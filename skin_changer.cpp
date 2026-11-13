#include "skin_changer.h"
#include "skin_changer_seh.h"
#include "../core/sdk_helpers.h"
#include "../core/game_state.h"
#include "../ui/theme.h"
#include "../imgui/imgui.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace SkinChanger
{
    static std::string s_godName;
    static std::string s_godKey;
    static std::vector<SkinEntry> s_skins;
    static bool s_scanned = false;
    static std::string s_status;
    static int s_selected = 0;
    static bool s_trackingApplyStatus = false;
    static std::string s_applyLabel;
    static std::string s_lastDiagnosticCopy;
    static std::string s_iconReport;

    static bool IContains(const std::string& hay, const char* needle)
    {
        if (!needle || !*needle) return false;
        std::string h = hay, n = needle;
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return h.find(n) != std::string::npos;
    }

    static bool GodKeysMatch(const std::string& a, const std::string& b)
    {
        if (a.empty() || b.empty()) return false;
        if (IContains(a, b.c_str()) || IContains(b, a.c_str())) return true;
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }

    static std::string StripPrefix(const std::string& in)
    {
        static const char* kPrefixes[] = {
            "CharacterItem_", "ZCharacter_", "ZSkin_", "SkinItem_",
            "BP_GOD_", "BP_", "ABP_", "CI_", "MESH_GOD_", "HWValiant_"
        };
        for (const char* pre : kPrefixes)
        {
            size_t len = strlen(pre);
            if (in.size() > len && _strnicmp(in.c_str(), pre, (int)len) == 0)
                return in.substr(len);
        }
        return in;
    }

    static std::string NormalizeGodKey(const std::string& raw)
    {
        if (raw.empty()) return {};
        std::string name = StripPrefix(raw);
        size_t skinPos = name.find("_Skin");
        if (skinPos == std::string::npos) skinPos = name.find("_S0");
        if (skinPos != std::string::npos)
            name = name.substr(0, skinPos);
        size_t dot = name.find('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        while (!name.empty() && (name.back() == '_' || (name.back() >= '0' && name.back() <= '9')))
            name.pop_back();
        if (name.size() > 2)
        {
            size_t pos = name.find_last_of('_');
            if (pos != std::string::npos && (name.size() - pos) <= 2)
                name = name.substr(0, pos);
        }
        return name;
    }

    static std::string GodKeyFromObject(SDK::UObject* obj)
    {
        char nameBuf[128]{};
        if (!SkinSeh::CopyObjectName(obj, nameBuf, sizeof(nameBuf)))
            return {};
        return NormalizeGodKey(nameBuf);
    }

    static std::string DetectActiveGodKey()
    {
        const SDK::UHWCharacterItem* item = SkinSeh::ResolveCharacterItemPtr();
        if (item)
        {
            std::string key = GodKeyFromObject(const_cast<SDK::UObject*>(
                static_cast<const SDK::UObject*>(item)));
            if (!key.empty())
                return key;
        }

        SDK::AHWCharacter_Base* live = SkinSeh::GetLiveGodPawn();
        if (live)
        {
            std::string key = GodKeyFromObject(live);
            if (!key.empty())
                return key;
        }

        return {};
    }

    static std::string GetSkinLabel(SDK::UHWSkinItem* skin, const char* fallback)
    {
        if (!skin || !SkinSeh::IsValidSkin(skin))
            return fallback ? fallback : "Skin";

        SDK::FText display = skin->GetItemDisplayNameCAPS();
        std::string friendly = display.ToString();
        if (!friendly.empty())
            return friendly;

        return fallback && fallback[0] ? fallback : "Skin";
    }

    static void AddSkinEntry(std::unordered_set<uintptr_t>& seen,
                             std::vector<SkinEntry>& out,
                             SDK::UHWSkinItem* skin,
                             const char* label)
    {
        if (!skin || !SkinSeh::IsValidSkin(skin)) return;
        uintptr_t key = reinterpret_cast<uintptr_t>(skin);
        if (!seen.insert(key).second) return;

        char nameBuf[128]{};
        if (!label || !label[0])
        {
            if (!SkinSeh::CopyObjectName(skin, nameBuf, sizeof(nameBuf)))
                return;
            label = nameBuf;
        }

        out.push_back({ GetSkinLabel(skin, label), skin });
    }

    void ResetSession()
    {
        SkinSeh::CancelLoadedIconScan();
        s_trackingApplyStatus = false;
        s_applyLabel.clear();
        s_iconReport.clear();
        s_skins.clear();
        s_godName.clear();
        s_godKey.clear();
        s_scanned = false;
        s_selected = 0;
        s_status = "Session ended — enter a match and rescan.";
        SkinSeh::ClearPendingApply();
    }

    void Scan()
    {
        SkinSeh::CancelLoadedIconScan();
        s_iconReport.clear();
        s_trackingApplyStatus = false;
        s_skins.clear();
        s_scanned = true;
        s_status.clear();

        const std::string activeGod = DetectActiveGodKey();
        s_godKey = activeGod;
        s_godName = activeGod;

        const SDK::UHWCharacterItem* charItems[SkinSeh::kMaxCharItems]{};
        const int charItemCount = SkinSeh::CollectCharacterItemCandidates(charItems);
        if (s_godName.empty() && charItemCount > 0)
        {
            s_godName = GodKeyFromObject(const_cast<SDK::UObject*>(
                static_cast<const SDK::UObject*>(charItems[0])));
        }

        std::unordered_set<uintptr_t> seen;
        int fromSubsystem = 0;
        int fromItem = 0;
        int fromAssoc = 0;

        const SDK::UHWCharacterItem* primaryItem = charItemCount > 0 ? charItems[0] : nullptr;

        SkinSeh::RawSkinEntry subsystemRaw[SkinSeh::kMaxRawSkins]{};
        fromSubsystem = SkinSeh::ScanSkinsFromGodSubsystem(
            primaryItem, s_godName.c_str(), subsystemRaw, SkinSeh::kMaxRawSkins);
        for (int i = 0; i < fromSubsystem; ++i)
            AddSkinEntry(seen, s_skins, subsystemRaw[i].Skin, subsystemRaw[i].Name);

        for (int c = 0; c < charItemCount; ++c)
        {
            SkinSeh::RawSkinEntry itemRaw[SkinSeh::kMaxRawSkins]{};
            const int found = SkinSeh::ScanSkinsFromCharacterItem(charItems[c], itemRaw, SkinSeh::kMaxRawSkins);
            fromItem += found;
            for (int i = 0; i < found; ++i)
                AddSkinEntry(seen, s_skins, itemRaw[i].Skin, itemRaw[i].Name);
        }

        if (s_skins.empty())
        {
            SkinSeh::RawSkinEntry assocRaw[SkinSeh::kMaxRawSkins]{};
            fromAssoc = SkinSeh::ScanSkinsByAssociation(
                primaryItem,
                s_godName.c_str(),
                assocRaw,
                SkinSeh::kMaxRawSkins,
                512);
            for (int i = 0; i < fromAssoc; ++i)
                AddSkinEntry(seen, s_skins, assocRaw[i].Skin, assocRaw[i].Name);
        }

        int fromAssets = 0;
        if (s_skins.empty() && !s_godName.empty())
        {
            SkinSeh::RawSkinEntry assetRaw[128]{};
            fromAssets = SkinSeh::ScanLoadedSkinAssets(
                s_godName.c_str(), assetRaw, 128, 512);
            for (int i = 0; i < fromAssets; ++i)
                AddSkinEntry(seen, s_skins, assetRaw[i].Skin, assetRaw[i].Name);
        }

        std::sort(s_skins.begin(), s_skins.end(),
                  [](const SkinEntry& a, const SkinEntry& b) { return a.Label < b.Label; });

        if (s_selected >= (int)s_skins.size())
            s_selected = s_skins.empty() ? -1 : 0;

        if (s_godName.empty())
            s_status = "No god detected — pick your god or spawn in-game, then rescan.";
        else if (s_skins.empty())
            s_status = "God: " + s_godName + " — no skins found yet. Wait for full load-in, then rescan.";
        else
            s_status = "God: " + s_godName + " — found " + std::to_string(s_skins.size()) +
                       " skin(s) (subsystem=" + std::to_string(fromSubsystem) +
                       ", item=" + std::to_string(fromItem) +
                       ", assoc=" + std::to_string(fromAssoc) +
                       ", assets=" + std::to_string(fromAssets) +
                       "). Select a skin and press Check loaded icons.";
    }

    void ApplySelected(int /*index*/)
    {
        // Diagnostic build: never send a cosmetic request or change the model.
        s_status = "Apply disabled in icon diagnostic build.";
    }

    const std::string& CurrentGod() { return s_godName; }
    const std::vector<SkinEntry>& Skins() { return s_skins; }
    bool HasScanned() { return s_scanned; }
    const char* LastStatus() { return s_status.c_str(); }

    void TickPending()
    {
        // Do not run any of the existing periodic presentation/apply code.
        SkinSeh::ClearPendingApply();
    }

    void DrawMenu()
    {
        const bool playable = GameState::IsPlayable();

        if (!playable)
        {
            if (SkinSeh::LoadedIconScanRunning())
            {
                SkinSeh::CancelLoadedIconScan();
                s_status = "Check cancelled: match no longer playable.";
            }
            ImGui::TextColored(UITheme::palette.warning,
                "Enter a match / spawn as your god, then Rescan.");
            ImGui::Spacing();
            if (UITheme::ActionButton("rescan_lobby", UITheme::Icon::Load, "Rescan",
                                      UITheme::palette.accent, { 140.f, 34.f }))
                ResetSession();
            if (!s_status.empty())
                ImGui::TextWrapped("%s", s_status.c_str());
            /* Keep the list shell visible so the page isn't empty. */
            ImGui::Spacing();
            ImGui::BeginChild("##skin_list_empty", ImVec2(0.f, 220.f), true);
            ImGui::TextDisabled("No skins yet — become playable and Rescan.");
            ImGui::EndChild();
            return;
        }

        const std::string liveGod = DetectActiveGodKey();
        if (!liveGod.empty())
            s_godName = liveGod;

        if (!liveGod.empty() && liveGod != s_godKey)
        {
            s_godKey = liveGod;
            Scan();
        }
        else if (!HasScanned())
        {
            Scan();
        }

        const char* godLabel = s_godName.empty() ? "Unknown (pick/spawn as your god)" : s_godName.c_str();
        ImGui::TextColored(UITheme::palette.accentHi, "Current God:");
        ImGui::SameLine();
        ImGui::Text("%s", godLabel);
        ImGui::SameLine();
        ImGui::TextDisabled("(%d skins)", (int)s_skins.size());

        ImGui::Spacing();
        if (UITheme::ActionButton("rescan", UITheme::Icon::Load, "Rescan",
                                  UITheme::palette.accent, { 140.f, 34.f }))
            Scan();
        ImGui::SameLine();
        ImGui::BeginDisabled(s_selected < 0 || s_selected >= (int)s_skins.size());
        if (ImGui::Button("Check loaded icons"))
        {
            s_iconReport.clear();
            SkinSeh::StartLoadedIconScan(s_skins[s_selected].Item);
            s_status = "Checking loaded objects. Keep this page open.";
        }
        ImGui::EndDisabled();
        if (SkinSeh::LoadedIconScanRunning())
        {
            SkinSeh::TickLoadedIconScan();
            if (!SkinSeh::LoadedIconScanRunning())
            {
                char report[4096]{};
                SkinSeh::FormatLoadedIconReport(report, sizeof(report));
                s_iconReport = report;
                s_status = "Check finished or stopped. Copy the report below.";
            }
        }
        else if (s_status == "Checking loaded objects. Keep this page open.")
        {
            char report[4096]{};
            SkinSeh::FormatLoadedIconReport(report, sizeof(report));
            s_iconReport = report;
            s_status = "Check finished or stopped. Copy the report below.";
        }

        // Keep the outcome above the scrollable list so wrapped diagnostics
        // cannot disappear beneath it in a smaller menu window.
        ImGui::Spacing();
        if (!s_status.empty())
        {
            ImGui::TextWrapped("Status: %s", s_status.c_str());
            if (SkinSeh::LoadedIconScanRunning())
                ImGui::TextDisabled("Scanned %d / %d objects", SkinSeh::LoadedIconScanIndex(), SkinSeh::LoadedIconScanLimit());
            if (!s_iconReport.empty() && ImGui::SmallButton("Copy icon report"))
                ImGui::SetClipboardText(s_iconReport.c_str());
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Select skin");

        /* Full list selector — not a hidden combo. */
        const float listH = (std::max)(180.f, ImGui::GetContentRegionAvail().y - 48.f);
        ImGui::BeginChild("##skin_list", ImVec2(0.f, listH), true);
        if (s_skins.empty())
        {
            ImGui::TextDisabled("No skins found for this god.");
            ImGui::TextDisabled("Wait for full load-in, then Rescan.");
        }
        else
        {
            for (int i = 0; i < (int)s_skins.size(); ++i)
            {
                const bool selected = (s_selected == i);
                ImGui::PushID(i);
                if (ImGui::Selectable(s_skins[i].Label.c_str(), selected,
                                      0))
                {
                    if (s_selected != i)
                    {
                        SkinSeh::CancelLoadedIconScan();
                        s_iconReport.clear();
                        s_status = "Skin selected. Press Check loaded icons.";
                    }
                    s_selected = i;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        if (s_selected >= 0 && s_selected < (int)s_skins.size())
        {
            ImGui::TextColored(UITheme::palette.accentHi, "Selected:");
            ImGui::SameLine();
            ImGui::TextUnformatted(s_skins[s_selected].Label.c_str());
        }

    }
}
