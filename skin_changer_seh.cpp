#include "skin_changer_seh.h"
#include "../core/sdk_helpers.h"
#include <Windows.h>
#include <cstring>
#include <cstdio>

namespace SkinSeh
{
    // Same render/UI path as the existing skin module; this is a local invocation
    // result, never a server acknowledgement or proof of a visible mesh change.
    static const char* s_lastApplyStatus = "No skin request attempted.";
    // Diagnostics are snapshots of calls the original code already makes. They do
    // not represent server acknowledgement or proof that the mesh has changed.
    struct SkinDiagnostic
    {
        bool RpcInvoked = false;
        bool PawnFound = false;
        int LoadoutMatchedBefore = -1; // -1 = unknown, 0 = no, 1 = yes
        int LoadoutMatchedAfter = -1;
        bool ApparelComponentFound = false;
        bool ApparelFault = false;
        int PresentationPasses = 0;
        int SoftAssets = 0;
        int Resolved = 0;
        int Missing = 0;
        int ApplyAccepted = 0;
        int ApplyRejected = 0;
        int Pulses = 0;
        // 0 initial, 1 running, 2 client-field match, 3 timeout, 4 invalid skin.
        int PendingOutcome = 0;
    };
    static SkinDiagnostic s_diagnostic{};
    static char s_diagnosticText[640]{};
    static void UpdateDiagnosticStatus()
    {
        const char* pending[] = {"initial", "running", "stopped: local-field match", "timeout", "invalid skin"};
        const int outcome = s_diagnostic.PendingOutcome;
        auto match = [](int value) { return value < 0 ? "unknown" : value ? "yes" : "no"; };
        snprintf(s_diagnosticText, sizeof(s_diagnosticText),
            "RPC invoked=%s (server NOT verified); pawn=%s; loadout local match before/after=%s/%s; "
            "presentation passes=%d; apparel component=%s; last pass: soft=%d resolved=%d missing=%d apply-ok=%d apply-failed=%d fault=%s; "
            "retry pulses=%d; pending=%s. Model change NOT verified.",
            s_diagnostic.RpcInvoked ? "yes" : "no", s_diagnostic.PawnFound ? "yes" : "no",
            match(s_diagnostic.LoadoutMatchedBefore), match(s_diagnostic.LoadoutMatchedAfter),
            s_diagnostic.PresentationPasses, s_diagnostic.ApparelComponentFound ? "yes" : "no",
            s_diagnostic.SoftAssets, s_diagnostic.Resolved, s_diagnostic.Missing,
            s_diagnostic.ApplyAccepted, s_diagnostic.ApplyRejected, s_diagnostic.ApparelFault ? "yes" : "no",
            s_diagnostic.Pulses, outcome >= 0 && outcome <= 4 ? pending[outcome] : "unknown");
        s_lastApplyStatus = s_diagnosticText;
    }
    const char* LastApplyStatus() { return s_lastApplyStatus; }

    static void StripNamePath(char* name)
    {
        if (!name || !name[0]) return;
        char* slash = strrchr(name, '/');
        if (slash && slash[1])
            memmove(name, slash + 1, strlen(slash + 1) + 1);
    }

    static bool FNameToUtf8(const SDK::FName& fname, char* out, size_t outLen, bool stripPath = true)
    {
        out[0] = '\0';
        __try {
            if (fname.IsNone()) return false;

            wchar_t wbuf[128] = {};
            UC::FString fstr(wbuf, 0, 128);
            if (!SDK::FName::AppendString)
                SDK::FName::InitInternal();

            auto appendFn = reinterpret_cast<void(*)(const SDK::FName*, UC::FString&)>(
                SDK::FName::AppendString);
            SDK::InSDKUtils::CallGameFunction(appendFn, &fname, fstr);

            const wchar_t* w = fstr.CStr();
            if (!w || !*w) return false;
            if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen, nullptr, nullptr) <= 0)
                return false;

            if (stripPath) StripNamePath(out);
            return out[0] != '\0';
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    // Selection-time diagnostic only. This DOES NOT retrieve pixel data, load assets,
    // send RPCs, or apply a skin. Name conversion uses the existing SDK FName helper.
    static void DescribeIconInfo(SDK::UIconInfo* info, char* out, size_t outLen)
    {
        if (!out || outLen == 0) return;
        out[0] = '\0';
        __try {
            if (!info) { snprintf(out, outLen, "none"); return; }
            if (!Helpers::IsValidPtr(info) || !info->Class || !Helpers::IsValidPtr(info->Class)) {
                snprintf(out, outLen, "unreadable icon object"); return;
            }
            char className[128]{};
            if (!CopyObjectName(info->Class, className, sizeof(className))) {
                snprintf(out, outLen, "icon class name unavailable"); return;
            }
            if (_stricmp(className, "ImageIconInfo") != 0) {
                snprintf(out, outLen, "class=%s; path=unsupported icon type", className); return;
            }
            const auto* image = static_cast<const SDK::UImageIconInfo*>(info);
            const auto& path = image->IconImage;
            char package[256]{}, asset[128]{};
            bool hasPackage = FNameToUtf8(path.AssetPath.PackageName, package, sizeof(package), false);
            bool hasAsset = FNameToUtf8(path.AssetPath.AssetName, asset, sizeof(asset), false);
            snprintf(out, outLen, "class=%s; package=%s; asset=%s%s", className,
                     hasPackage ? package : "<unavailable>", hasAsset ? asset : "<unavailable>",
                     (hasPackage && hasAsset) ? "" : "; path not confirmed");
        } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(out, outLen, "icon metadata read fault"); }
    }

    void InspectSkinIcon(SDK::UHWSkinItem* skin, char* out, size_t outLen)
    {
        if (!out || outLen == 0) return;
        out[0] = '\0';
        __try {
            if (!skin || !Helpers::IsValidPtr(skin) || !IsValidSkin(skin)) {
                snprintf(out, outLen, "Icon diagnostic: selected skin unavailable; rescan."); return;
            }
            char skinName[128]{}, itemInfo[600]{};
            if (!CopyObjectName(skin, skinName, sizeof(skinName)))
                strncpy_s(skinName, "<unavailable>", _TRUNCATE);
            DescribeIconInfo(skin->ItemIconInfo, itemInfo, sizeof(itemInfo));
            const int count = skin->Icons.Num();
            if (count < 0 || count > 128) {
                snprintf(out, outLen, "Icon diagnostic: skin=%s; default=%s; icon list invalid (count=%d). No texture loaded.", skinName, itemInfo, count); return;
            }
            snprintf(out, outLen, "Icon diagnostic (metadata only; no texture loaded):\nskin=%s\ndefault=%s\nicon entries=%d", skinName, itemInfo, count);
            for (int i = 0; i < count && i < 6; ++i) {
                if (!skin->Icons.IsValidIndex(i)) break;
                const auto& ref = skin->Icons[i];
                char info[600]{};
                DescribeIconInfo(ref.IconInfo, info, sizeof(info));
                const size_t used = strlen(out);
                if (used + 1 >= outLen) break;
                snprintf(out + used, outLen - used, "\nentry[%d]=%s", i, info);
            }
            if (count > 6) {
                const size_t used = strlen(out);
                if (used + 1 < outLen) snprintf(out + used, outLen - used, "\nadditional entries omitted");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(out, outLen, "Icon diagnostic: metadata access fault; stop testing."); }
    }


    // A bounded scan spread over frames. Only FNames and text are retained; never
    // retain the discovered UObject or try to load an asset or access its pixels.
    struct IconProbeEntry
    {
        SDK::FName Package;
        SDK::FName Asset;
        char label[24]{};
        char assetName[128]{};
        char matchedClass[128]{};
        int sameName = 0;
        int pathMatches = 0;
        bool usable = false;
    };
    static IconProbeEntry s_probe[7]{};
    static int s_probeCount = 0, s_scanIndex = 0, s_scanLimit = 0;
    // 0 idle, 1 scanning, 2 complete, 3 error, 4 incomplete
    static int s_scanState = 0;
    static char s_probeSkin[128]{};
    static char s_probeProblem[160]{};

    void CancelLoadedIconScan() { s_scanState = 0; }
    bool LoadedIconScanRunning() { return s_scanState == 1; }
    int LoadedIconScanIndex() { return s_scanIndex; }
    int LoadedIconScanLimit() { return s_scanLimit; }

    static void AddProbeIcon(SDK::UIconInfo* info, const char* label)
    {
        if (s_probeCount >= 7) return;
        IconProbeEntry& item = s_probe[s_probeCount++];
        item = IconProbeEntry{};
        snprintf(item.label, sizeof(item.label), "%s", label);
        if (!info || !Helpers::IsValidPtr(info) || !info->Class || !Helpers::IsValidPtr(info->Class)) return;
        char cls[128]{};
        if (!CopyObjectName(info->Class, cls, sizeof(cls)) || _stricmp(cls, "ImageIconInfo") != 0) return;
        const auto& path = static_cast<SDK::UImageIconInfo*>(info)->IconImage.AssetPath;
        if (path.PackageName.IsNone() || path.AssetName.IsNone()) return;
        item.Package = path.PackageName;
        item.Asset = path.AssetName;
        if (!FNameToUtf8(item.Asset, item.assetName, sizeof(item.assetName), false))
            snprintf(item.assetName, sizeof(item.assetName), "<name unavailable>");
        item.usable = true;
    }

    void StartLoadedIconScan(SDK::UHWSkinItem* skin)
    {
        s_scanState = 0; s_probeCount = 0; s_scanIndex = 0; s_scanLimit = 0;
        s_probeSkin[0] = '\0'; s_probeProblem[0] = '\0';
        __try {
            if (!skin || !Helpers::IsValidPtr(skin) || !IsValidSkin(skin)) {
                snprintf(s_probeProblem, sizeof(s_probeProblem), "selected skin unavailable; rescan"); s_scanState = 3; return;
            }
            if (!CopyObjectName(skin, s_probeSkin, sizeof(s_probeSkin)))
                snprintf(s_probeSkin, sizeof(s_probeSkin), "<unavailable>");
            AddProbeIcon(skin->ItemIconInfo, "default");
            const int count = skin->Icons.Num();
            if (count < 0 || count > 128) {
                snprintf(s_probeProblem, sizeof(s_probeProblem), "invalid icon count=%d", count); s_scanState = 3; return;
            }
            for (int i = 0; i < count && i < 6; ++i) {
                if (!skin->Icons.IsValidIndex(i)) break;
                char label[24]{}; snprintf(label, sizeof(label), "entry[%d]", i);
                AddProbeIcon(skin->Icons[i].IconInfo, label);
            }
            bool any = false;
            for (int i = 0; i < s_probeCount; ++i) if (s_probe[i].usable) any = true;
            if (!any) { snprintf(s_probeProblem, sizeof(s_probeProblem), "no usable ImageIconInfo paths"); s_scanState = 3; return; }
            auto* objs = SDK::UObject::GObjects.GetTypedPtr();
            if (!objs || !Helpers::IsValidPtr(objs)) {
                snprintf(s_probeProblem, sizeof(s_probeProblem), "object array unavailable"); s_scanState = 3; return;
            }
            s_scanLimit = objs->Num();
            if (s_scanLimit <= 0 || s_scanLimit > 10000000) {
                snprintf(s_probeProblem, sizeof(s_probeProblem), "invalid object count=%d", s_scanLimit); s_scanState = 3; return;
            }
            s_scanState = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            snprintf(s_probeProblem, sizeof(s_probeProblem), "metadata access fault; stop testing"); s_scanState = 3;
        }
    }

    void TickLoadedIconScan()
    {
        if (s_scanState != 1) return;
        __try {
            auto* objs = SDK::UObject::GObjects.GetTypedPtr();
            if (!objs || !Helpers::IsValidPtr(objs)) {
                snprintf(s_probeProblem, sizeof(s_probeProblem), "object array unavailable"); s_scanState = 3; return;
            }
            const int liveCount = objs->Num();
            if (liveCount < s_scanLimit || liveCount > 10000000) {
                snprintf(s_probeProblem, sizeof(s_probeProblem), "object count changed or invalid; scan incomplete"); s_scanState = 4; return;
            }
            const int end = (s_scanIndex + 1000 < s_scanLimit) ? s_scanIndex + 1000 : s_scanLimit;
            for (; s_scanIndex < end; ++s_scanIndex) {
                SDK::UObject* obj = objs->GetByIndex(s_scanIndex);
                if (!obj || !Helpers::IsValidPtr(obj)) continue;
                const SDK::FName name = obj->Name;
                for (int j = 0; j < s_probeCount; ++j) {
                    IconProbeEntry& item = s_probe[j];
                    if (!item.usable || name != item.Asset) continue;
                    ++item.sameName;
                    if (!obj->Outer || !Helpers::IsValidPtr(obj->Outer) || obj->Outer->Name != item.Package) continue;
                    ++item.pathMatches;
                    if (obj->Class && Helpers::IsValidPtr(obj->Class)) {
                        if (!CopyObjectName(obj->Class, item.matchedClass, sizeof(item.matchedClass)))
                            snprintf(item.matchedClass, sizeof(item.matchedClass), "<unavailable>");
                    }
                }
            }
            if (s_scanIndex >= s_scanLimit) s_scanState = 2;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            snprintf(s_probeProblem, sizeof(s_probeProblem), "object access fault; stop testing"); s_scanState = 3;
        }
    }

    void FormatLoadedIconReport(char* out, size_t outLen)
    {
        if (!out || outLen == 0) return;
        const char* state = s_scanState == 1 ? "running" : s_scanState == 2 ? "completed" :
                            s_scanState == 3 ? "error" : s_scanState == 4 ? "incomplete" : "idle";
        snprintf(out, outLen, "Loaded icon object scan (no loading or pixel access):\nskin=%s\nstate=%s; scanned=%d/%d%s%s",
                 s_probeSkin[0] ? s_probeSkin : "<unavailable>", state, s_scanIndex, s_scanLimit,
                 s_probeProblem[0] ? "; detail=" : "", s_probeProblem);
        for (int i = 0; i < s_probeCount; ++i) {
            const IconProbeEntry& item = s_probe[i];
            const size_t used = strlen(out); if (used + 1 >= outLen) break;
            if (!item.usable)
                snprintf(out + used, outLen - used, "\n%s: path unavailable or unsupported", item.label);
            else
                snprintf(out + used, outLen - used, "\n%s: asset=%s; exact-path objects=%d; class=%s; same-name objects=%d",
                         item.label, item.assetName, item.pathMatches,
                         item.matchedClass[0] ? item.matchedClass : "<not observed>", item.sameName);
        }
        const size_t used = strlen(out);
        if (used + 1 < outLen) snprintf(out + used, outLen - used,
            "\nSnapshot only: objects may change during scan; no result does not prove unavailable; rendering not verified.");
    }

    static bool IsRejectedSkinName(const char* name)
    {
        if (!name || !name[0]) return true;
        if (_stricmp(name, "CoreUObject") == 0) return true;
        if (_stricmp(name, "Object") == 0) return true;
        if (_stricmp(name, "HWSkinItem") == 0) return true;
        if (_strnicmp(name, "Default__", 9) == 0) return true;
        return false;
    }

    bool CopyObjectName(SDK::UObject* obj, char* out, size_t outLen)
    {
        out[0] = '\0';
        __try {
            if (!obj || !Helpers::IsValidPtr(obj)) return false;

            SDK::FName fname = obj->Name;
            if (fname.IsNone()) return false;

            wchar_t wbuf[128] = {};
            UC::FString fstr(wbuf, 0, 128);
            if (!SDK::FName::AppendString)
                SDK::FName::InitInternal();

            auto appendFn = reinterpret_cast<void(*)(const SDK::FName*, UC::FString&)>(
                SDK::FName::AppendString);
            SDK::InSDKUtils::CallGameFunction(appendFn, &fname, fstr);

            const wchar_t* w = fstr.CStr();
            if (!w || !*w) return false;
            if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen, nullptr, nullptr) <= 0)
                return false;

            StripNamePath(out);
            return out[0] != '\0';
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    bool IsValidSkin(SDK::UObject* obj)
    {
        __try {
            if (!obj || !Helpers::IsValidPtr(obj)) return false;
            if (!obj->IsA(SDK::UHWSkinItem::StaticClass())) return false;
            if (obj->IsDefaultObject()) return false;

            char name[128]{};
            if (!CopyObjectName(obj, name, sizeof(name))) return false;
            return !IsRejectedSkinName(name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    static bool NamesEqualInsensitive(const char* a, const char* b)
    {
        return a && b && a[0] && b[0] && _stricmp(a, b) == 0;
    }

    static bool GodKeyMatchesName(const char* godKey, const char* objectName)
    {
        if (!godKey || !godKey[0] || !objectName || !objectName[0]) return false;
        if (strlen(godKey) < 2) return false;
        if (NamesEqualInsensitive(objectName, godKey)) return true;

        char lowerName[128]{};
        char lowerKey[64]{};
        strncpy_s(lowerName, objectName, _TRUNCATE);
        strncpy_s(lowerKey, godKey, _TRUNCATE);
        _strlwr_s(lowerName);
        _strlwr_s(lowerKey);
        return strstr(lowerName, lowerKey) != nullptr;
    }

    SDK::AHWCharacter_Base* GetLiveGodPawn()
    {
        __try {
            auto* ch = Helpers::GetLocalChar();
            if (Helpers::IsGodCharacter(ch))
                return ch;

            auto* pc = Helpers::GetLocalPC();
            if (!pc || !pc->PlayerState) return nullptr;
            if (!pc->PlayerState->IsA(SDK::AHWPlayerState::StaticClass()))
                return nullptr;

            auto* ps = static_cast<SDK::AHWPlayerState*>(pc->PlayerState);
            SDK::AHWCharacter_Base* base = ps->GetCharacterBase();
            if (Helpers::IsGodCharacter(base))
                return base;

            SDK::APawn* pawn = pc->K2_GetPawn();
            if (pawn && pawn->IsA(SDK::AHWCharacter_Base::StaticClass()))
            {
                auto* hw = static_cast<SDK::AHWCharacter_Base*>(pawn);
                if (Helpers::IsGodCharacter(hw))
                    return hw;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    SDK::AHWPlayerState* GetLocalPlayerState()
    {
        __try {
            auto* ch = Helpers::GetLocalChar();
            if (ch)
            {
                auto* ps = ch->GetHWPlayerState();
                if (ps) return ps;
            }

            auto* pc = Helpers::GetLocalPC();
            if (!pc || !pc->PlayerState) return nullptr;
            if (!pc->PlayerState->IsA(SDK::AHWPlayerState::StaticClass())) return nullptr;
            return static_cast<SDK::AHWPlayerState*>(pc->PlayerState);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    static const SDK::UHWCharacterItem* TryCharacterItemFromAsa(SDK::AHWCharacterAbilitySystemActor* asa)
    {
        __try {
            if (!asa || !Helpers::IsValidPtr(asa)) return nullptr;

            auto* loadout = asa->GetLoadoutComponent();
            if (loadout)
            {
                const SDK::UHWCharacterItem* item = loadout->GetCurrentCharacterItem();
                if (item && Helpers::IsValidPtr(item)) return item;
            }

            const SDK::UHWCharacterItem* item = asa->GetCurrentCharacterItem();
            if (item && Helpers::IsValidPtr(item)) return item;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    const SDK::UHWCharacterItem* ResolveCharacterItemPtr()
    {
        __try {
            SDK::AHWCharacter_Base* live = GetLiveGodPawn();
            if (live)
            {
                auto* asa = live->GetCharacterAbilitySystemActor();
                const SDK::UHWCharacterItem* item = TryCharacterItemFromAsa(asa);
                if (item) return item;
            }

            SDK::AHWPlayerState* ps = GetLocalPlayerState();
            if (ps)
            {
                const SDK::UHWCharacterItem* item = TryCharacterItemFromAsa(ps->GetCharacterAbilitySystemActor());
                if (item) return item;

                const SDK::UHWCharacterItem* draft = ps->GetDraftCharacterChoice();
                if (draft && Helpers::IsValidPtr(draft)) return draft;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    static void PushUniqueCharItem(const SDK::UHWCharacterItem* item,
                                   const SDK::UHWCharacterItem* outItems[kMaxCharItems],
                                   int& count)
    {
        if (!item || !Helpers::IsValidPtr(item) || count >= kMaxCharItems) return;
        for (int i = 0; i < count; ++i)
        {
            if (outItems[i] == item) return;
        }
        outItems[count++] = item;
    }

    int CollectCharacterItemCandidates(const SDK::UHWCharacterItem* outItems[kMaxCharItems])
    {
        int count = 0;
        __try {
            SDK::AHWCharacter_Base* live = GetLiveGodPawn();
            if (live)
            {
                auto* asa = live->GetCharacterAbilitySystemActor();
                PushUniqueCharItem(TryCharacterItemFromAsa(asa), outItems, count);
            }

            SDK::AHWPlayerState* ps = GetLocalPlayerState();
            if (ps)
            {
                PushUniqueCharItem(TryCharacterItemFromAsa(ps->GetCharacterAbilitySystemActor()), outItems, count);
                PushUniqueCharItem(ps->GetDraftCharacterChoice(), outItems, count);
            }

            PushUniqueCharItem(ResolveCharacterItemPtr(), outItems, count);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return count;
    }

    static SDK::UHWSkinItem* ResolveSoftSkin(const SDK::TSoftObjectPtr<SDK::UHWSkinItem>& soft)
    {
        SDK::UHWSkinItem* direct = soft.Get();
        if (IsValidSkin(direct)) return direct;

        const SDK::TSoftObjectPtr<SDK::UObject>& softObj =
            reinterpret_cast<const SDK::TSoftObjectPtr<SDK::UObject>&>(soft);
        SDK::UObject* resolved = SDK::UKismetSystemLibrary::Conv_SoftObjectReferenceToObject(softObj);
        if (IsValidSkin(resolved))
            return static_cast<SDK::UHWSkinItem*>(resolved);

        char assetName[128]{};
        if (FNameToUtf8(soft.ObjectID.AssetPath.AssetName, assetName, sizeof(assetName)))
        {
            SDK::UHWSkinItem* found = SDK::UObject::FindObjectFast<SDK::UHWSkinItem>(assetName);
            if (IsValidSkin(found)) return found;
        }

        return nullptr;
    }

    static bool SkinMatchesCharacter(SDK::UHWSkinItem* skin,
                                     const SDK::UHWCharacterItem* charItem,
                                     const char* godKey)
    {
        __try {
            if (!IsValidSkin(skin)) return false;

            if (charItem)
            {
                const SDK::UHWCharacterItem* assoc = skin->GetAssociatedCharacter().Get();
                if (assoc && assoc == charItem) return true;

                if (assoc)
                {
                    char skinChar[128]{};
                    char targetChar[128]{};
                    if (CopyObjectName(const_cast<SDK::UObject*>(static_cast<const SDK::UObject*>(assoc)),
                                       skinChar, sizeof(skinChar)) &&
                        CopyObjectName(const_cast<SDK::UObject*>(static_cast<const SDK::UObject*>(charItem)),
                                       targetChar, sizeof(targetChar)) &&
                        (NamesEqualInsensitive(skinChar, targetChar) ||
                         GodKeyMatchesName(targetChar, skinChar) ||
                         GodKeyMatchesName(skinChar, targetChar)))
                    {
                        return true;
                    }
                }
            }

            if (godKey && godKey[0])
            {
                char skinName[128]{};
                if (CopyObjectName(skin, skinName, sizeof(skinName)))
                    return GodKeyMatchesName(godKey, skinName);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    static void PushRawSkin(RawSkinEntry* out, int maxOut, int& added, SDK::UHWSkinItem* skin)
    {
        if (!out || !skin || added >= maxOut || !IsValidSkin(skin)) return;

        for (int i = 0; i < added; ++i)
        {
            if (out[i].Skin == skin) return;
        }

        if (!CopyObjectName(skin, out[added].Name, sizeof(out[added].Name)))
            return;

        out[added].Skin = skin;
        ++added;
    }

    static SDK::TArray<SDK::UHWSkinItem*>* GetSubsystemSkinArray(SDK::UHWGodSubsystem* sys, bool active)
    {
        if (!sys) return nullptr;
        const uintptr_t offset = active ? 0x00B0 : 0x00A0;
        return reinterpret_cast<SDK::TArray<SDK::UHWSkinItem*>*>(
            reinterpret_cast<uint8_t*>(sys) + offset);
    }

    int ScanSkinsFromGodSubsystem(const SDK::UHWCharacterItem* charItem,
                                  const char* godKey,
                                  RawSkinEntry* out,
                                  int maxOut)
    {
        if (!out || maxOut <= 0) return 0;
        int added = 0;

        __try {
            auto* world = Helpers::GetWorld();
            if (!world) return 0;

            auto* godSys = SDK::UHWLibrary_Core::GetGodSubsystem(world);
            if (!godSys) return 0;

            const bool useActive[] = { false, true };
            for (bool active : useActive)
            {
                SDK::TArray<SDK::UHWSkinItem*>* skins = GetSubsystemSkinArray(godSys, active);
                if (!skins || !skins->IsValid()) continue;

                const int count = skins->Num();
                for (int i = 0; i < count && added < maxOut; ++i)
                {
                    SDK::UHWSkinItem* skin = (*skins)[i];
                    if (!SkinMatchesCharacter(skin, charItem, godKey)) continue;
                    PushRawSkin(out, maxOut, added, skin);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return added;
    }

    int ScanSkinsFromCharacterItem(const SDK::UHWCharacterItem* charItem,
                                   RawSkinEntry* out,
                                   int maxOut)
    {
        if (!out || maxOut <= 0) return 0;
        int added = 0;

        __try {
            if (!charItem || !Helpers::IsValidPtr(charItem)) return 0;

            charItem->UpdateAssociatedSkinItems();

            SDK::TArray<SDK::TSoftObjectPtr<SDK::UHWSkinItem>> skinPtrs{};
            charItem->GetSkinItemsForCharacterItem(&skinPtrs);

            const int count = skinPtrs.Num();
            for (int i = 0; i < count && added < maxOut; ++i)
            {
                SDK::UHWSkinItem* skin = ResolveSoftSkin(skinPtrs[i]);
                if (!skin) continue;
                PushRawSkin(out, maxOut, added, skin);
            }

            SDK::UHWSkinItem* defaultSkin = ResolveSoftSkin(charItem->DefaultSkin);
            if (defaultSkin)
                PushRawSkin(out, maxOut, added, defaultSkin);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return added;
    }

    int ScanSkinsByAssociation(const SDK::UHWCharacterItem* charItem,
                               const char* godKey,
                               RawSkinEntry* out,
                               int maxOut,
                               int maxAdds)
    {
        if (!out || maxOut <= 0 || maxAdds <= 0) return 0;
        int added = 0;

        SDK::UHWSkinItem* loaded[kMaxLoadedSkins]{};
        const int loadedCount = 0; // unused, kept for future expansion

        __try {
            auto* objs = SDK::UObject::GObjects.GetTypedPtr();
            if (!objs) return 0;

            SDK::UClass* skinClass = SDK::UHWSkinItem::StaticClass();
            if (!skinClass) return 0;

            const int total = objs->Num();
            for (int i = 0; i < total && added < maxOut && added < maxAdds; ++i)
            {
                SDK::UObject* obj = objs->GetByIndex(i);
                if (!IsValidSkin(obj)) continue;
                if (!obj->IsA(skinClass)) continue;

                SDK::UHWSkinItem* skin = static_cast<SDK::UHWSkinItem*>(obj);
                if (!SkinMatchesCharacter(skin, charItem, godKey)) continue;
                PushRawSkin(out, maxOut, added, skin);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            (void)loaded;
            (void)loadedCount;
        }

        return added;
    }

    int ScanLoadedSkinAssets(const char* godFilterKey,
                             RawSkinEntry* out,
                             int maxOut,
                             int maxAdds)
    {
        return ScanSkinsByAssociation(nullptr, godFilterKey, out, maxOut, maxAdds);
    }

    // Icon-only diagnostic build: remove all callable skin mutation paths.
    void ClearPendingApply() {}
    void TickPendingApply() {}
    bool ApplySkinToLoadout(SDK::UHWSkinItem*, const SDK::UHWCharacterItem*)
    {
        s_lastApplyStatus = "Apply disabled in icon diagnostic build.";
        return false;
    }
}
