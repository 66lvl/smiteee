#include "cooldown_window.h"
#include "esp_cooldown_seh.h"
#include "ability_icons.h"
#include "ability_cd_ui.h"
#include "../core/sdk_helpers.h"
#include "../core/game_state.h"
#include "../core/pawn_filter.h"
#include "../ui/theme.h"
#include "../ui/menu.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace CooldownWindow
{
    Config cfg;

    static constexpr int   kMaxEnemyGods   = 5;
    static constexpr int   kMaxTeamGods    = 5;
    static constexpr int   kMaxWidgetSlots = kMaxEnemyGods + kMaxTeamGods;
    static constexpr float kSectionDividerH   = 18.f;
    static constexpr float kHubTileGap       = 4.f;
    static constexpr float kRelicTileGap     = 3.f;
    static constexpr float kDistAfterTiles   = 6.f;
    static constexpr float kDistPillW        = 48.f;
    static constexpr float kTopChromeH       = 40.f;
    static constexpr float kBottomPad        = 6.f;
    static constexpr float kSidePad          = 10.f;
    static constexpr float kHubStackGap      = 12.f;
    static constexpr float kHubTopY          = 72.f;
    static constexpr float kHubRightPad      = 18.f;

    struct GodRow
    {
        uintptr_t   sortKey = 0;
        std::string name;
        std::string godCode;
        float       distance = 0.f;
        bool        isAlly   = false;
        EspCooldownSeh::EspAbilitySlotInfo slots[EspCooldownSeh::kMaxCooldowns]{};
        EspCooldownSeh::EspAbilitySlotInfo relicSlots[EspCooldownSeh::kMaxRelicSlots]{};
        std::string relicCode;
    };

    static bool s_combinedHubOpen = true;
    static bool s_enemyHubOpen    = true;
    static bool s_teamHubOpen     = true;

    static ImVec2 s_seedCombinedPos{-999.f, -999.f};
    static ImVec2 s_seedEnemyPos{-999.f, -999.f};
    static ImVec2 s_seedTeamPos{-999.f, -999.f};

    static ImVec2 s_widgetPos[kMaxWidgetSlots];
    static ImVec2 s_widgetSeed[kMaxWidgetSlots];

    static void ClearWidgetSlotPositions()
    {
        for (int i = 0; i < kMaxWidgetSlots; ++i)
        {
            s_widgetPos[i] = ImVec2(-1.f, -1.f);
            s_widgetSeed[i] = ImVec2(-1.f, -1.f);
        }
    }
    static bool s_widgetLayoutLoaded = false;

    static float HubTileSizeClamped()
    {
        return std::clamp(cfg.HubTileSize, 24.f, 56.f);
    }

    static float WidgetGodNameSizeClamped()
    {
        return std::clamp(cfg.WidgetGodNameSize, 10.f, 28.f);
    }

    static AbilityCdUi::HubSlotFilter BuildHubSlotFilter()
    {
        AbilityCdUi::HubSlotFilter filter{};
        filter.showName = cfg.ShowGodName;
        filter.showDistance = cfg.ShowDistance;
        filter.showAbility[0] = cfg.ShowAbility1;
        filter.showAbility[1] = cfg.ShowAbility2;
        filter.showAbility[2] = cfg.ShowAbility3;
        filter.showAbility[3] = cfg.ShowAbility4;
        filter.showRelic = cfg.ShowRelic;
        return filter;
    }

    static AbilityCdUi::HubDrawStyle BuildRowStyle(bool transparent, bool isAlly, bool vertical)
    {
        static AbilityCdUi::HubSlotFilter s_filter;
        s_filter = BuildHubSlotFilter();

        AbilityCdUi::HubDrawStyle style{};
        style.transparentBackground = transparent;
        style.isAlly = isAlly;
        style.verticalLayout = vertical;
        style.godNameFontSize = transparent ? WidgetGodNameSizeClamped() : 15.f;
        style.slotFilter = &s_filter;
        return style;
    }

    static void SyncHubTileSizeFromWindow(float contentWidth,
                                          bool vertical,
                                          const AbilityCdUi::HubSlotFilter& filter)
    {
        if (!Menu::WantsInput() || contentWidth < 60.f)
            return;

        // Only scale tiles while the user is actively drag-resizing a hub window.
        // Otherwise this overwrites the Icon Size slider every frame the menu is open.
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window || window->ResizeBorderHeld < 0)
            return;

        const float measured =
            AbilityCdUi::MeasureHubRowWidth(HubTileSizeClamped(), vertical, &filter);
        if (measured < 40.f)
            return;

        cfg.HubTileSize = std::clamp(HubTileSizeClamped() * (contentWidth / measured),
                                     24.f, 56.f);
    }

    static ImGuiWindowFlags HubWindowFlags(bool allowResize)
    {
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoSavedSettings;

        if (!allowResize)
            flags |= ImGuiWindowFlags_NoResize;

        if (!Menu::WantsInput())
            flags |= ImGuiWindowFlags_NoInputs;

        return flags;
    }

    static float MeasureHubWidth()
    {
        const bool vertical = cfg.WidgetMode && cfg.WidgetVerticalLayout;
        const AbilityCdUi::HubSlotFilter filter = BuildHubSlotFilter();
        return kSidePad * 2.f +
               AbilityCdUi::MeasureHubRowWidth(HubTileSizeClamped(), vertical, &filter);
    }

    static float MeasureHubHeightEx(int godCount, bool hasEmptyMessage,
                                      int sectionDividers, bool includeTopChrome)
    {
        const float padY = ImGui::GetStyle().WindowPadding.y * 2.f;
        if (godCount <= 0)
        {
            const float chrome = includeTopChrome ? kTopChromeH : 0.f;
            return chrome + (hasEmptyMessage ? 34.f : 0.f) + kBottomPad + padY + 4.f;
        }

        const bool vertical = cfg.WidgetMode && cfg.WidgetVerticalLayout;
        const AbilityCdUi::HubSlotFilter filter = BuildHubSlotFilter();
        const float rowH =
            AbilityCdUi::HubRowMetrics(HubTileSizeClamped(), vertical, &filter).rowHeight;
        const float chrome = includeTopChrome ? kTopChromeH : 0.f;
        return chrome + godCount * rowH + sectionDividers * kSectionDividerH +
               kBottomPad + padY + 12.f;
    }

    static void PushWindowStyle()
    {
        if (cfg.TransparentBackground)
        {
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.08f, 0.94f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.12f));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.95f, 0.98f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, cfg.TransparentBackground ? 0.f : 12.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kSidePad, 8.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    }

    static void PopWindowStyle()
    {
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
    }

    static int CountSectionDividers(const std::vector<GodRow>& rows)
    {
        int count = 0;
        for (int i = 1; i < (int)rows.size(); ++i)
        {
            if (rows[i].isAlly && !rows[i - 1].isAlly)
                ++count;
        }
        return count;
    }

    static float MeasureHubHeight(int godCount, bool hasEmptyMessage,
                                  int sectionDividers)
    {
        return MeasureHubHeightEx(godCount, hasEmptyMessage, sectionDividers, true);
    }

    static void ParseWidgetLayout(const std::string& layout)
    {
        ClearWidgetSlotPositions();
        if (layout.empty())
            return;

        size_t start = 0;
        while (start < layout.size())
        {
            size_t sep = layout.find(':', start);
            if (sep == std::string::npos)
                break;

            const std::string key = layout.substr(start, sep - start);
            size_t end = layout.find(';', sep + 1);
            if (end == std::string::npos)
                end = layout.size();

            int slot = -1;
            float x = -1.f;
            float y = -1.f;
            const std::string coords = layout.substr(sep + 1, end - sep - 1);
            if (sscanf_s(key.c_str(), "%d", &slot) == 1 &&
                slot >= 0 && slot < kMaxWidgetSlots &&
                sscanf_s(coords.c_str(), "%f,%f", &x, &y) == 2 &&
                x >= 0.f && y >= 0.f)
            {
                s_widgetPos[slot] = ImVec2(x, y);
            }

            start = (end < layout.size()) ? end + 1 : layout.size();
        }
    }

    void CaptureWidgetLayout()
    {
        std::string out;
        out.reserve(384);
        for (int i = 0; i < kMaxWidgetSlots; ++i)
        {
            if (s_widgetPos[i].x < 0.f || s_widgetPos[i].y < 0.f)
                continue;
            if (!out.empty())
                out += ';';
            char buf[64]{};
            snprintf(buf, sizeof(buf), "%d:%.1f,%.1f",
                     i, s_widgetPos[i].x, s_widgetPos[i].y);
            out += buf;
        }
        cfg.WidgetLayout = std::move(out);
    }

    void ResetWidgetLayoutCache()
    {
        s_widgetLayoutLoaded = false;
        ClearWidgetSlotPositions();
    }

    static void EnsureWidgetLayoutLoaded()
    {
        if (s_widgetLayoutLoaded)
            return;
        ParseWidgetLayout(cfg.WidgetLayout);
        s_widgetLayoutLoaded = true;
    }

    static float MeasureWidgetHeight()
    {
        const bool vertical = cfg.WidgetVerticalLayout;
        const AbilityCdUi::HubSlotFilter filter = BuildHubSlotFilter();
        const float nameSize = WidgetGodNameSizeClamped();
        const float rowH =
            AbilityCdUi::HubRowMetrics(HubTileSizeClamped(), vertical, &filter, nameSize)
                .rowHeight;
        const float padY = ImGui::GetStyle().WindowPadding.y * 2.f;
        return rowH + padY + 4.f;
    }

    static ImVec2 DefaultWidgetPos(int slotIndex)
    {
        ImGuiIO& io = ImGui::GetIO();
        const float rowH = MeasureWidgetHeight() + 4.f;
        const float hubWidth = MeasureHubWidth();
        const bool isTeamSlot = slotIndex >= kMaxEnemyGods;
        const int stackIndex = isTeamSlot ? slotIndex - kMaxEnemyGods : slotIndex;
        const float x = isTeamSlot
            ? kHubRightPad
            : (io.DisplaySize.x - hubWidth - kHubRightPad);
        return ImVec2(x, kHubTopY + stackIndex * rowH);
    }

    static void DrawSectionDivider(ImDrawList* dl, const ImVec2& min, float width, const char* label)
    {
        const float y = min.y + kSectionDividerH * 0.5f;
        const ImU32 lineCol = IM_COL32(255, 255, 255, 72);

        ImFont* smallFont = UITheme::fontSmall ? UITheme::fontSmall : ImGui::GetFont();
        const ImVec2 textSize = smallFont->CalcTextSizeA(10.f, FLT_MAX, 0.f, label);
        const float textX = min.x + (width - textSize.x) * 0.5f;
        const float textY = min.y + (kSectionDividerH - textSize.y) * 0.5f;
        const float gap = 10.f;

        dl->AddLine({ min.x, y }, { textX - gap, y }, lineCol, 1.f);
        dl->AddLine({ textX + textSize.x + gap, y }, { min.x + width, y }, lineCol, 1.f);
        dl->AddText(smallFont, 10.f, { textX, textY },
                    IM_COL32(150, 158, 174, 220), label);
    }

    static void DrawHubChrome(ImDrawList* dl,
                              const ImVec2& wpos,
                              const ImVec2& wsize,
                              int godCount,
                              const char* title,
                              bool transparent)
    {
        const ImVec2 innerMin(wpos.x + 1.f, wpos.y + 1.f);
        const ImVec2 innerMax(wpos.x + wsize.x - 1.f, wpos.y + wsize.y - 1.f);

        if (!transparent)
        {
            dl->AddRectFilled(innerMin, innerMax, IM_COL32(10, 12, 18, 235), 12.f);
            dl->AddRect(innerMin, innerMax, IM_COL32(255, 255, 255, 35), 12.f, 0, 1.f);
            dl->AddRectFilled(
                { innerMin.x, innerMin.y },
                { innerMax.x, innerMin.y + 3.f },
                IM_COL32(255, 255, 255, 16), 12.f, ImDrawFlags_RoundCornersTop);

            const ImVec2 accentMin(innerMin.x + 14.f, innerMin.y + 12.f);
            const ImVec2 accentMax(innerMin.x + 18.f, innerMin.y + 28.f);
            dl->AddRectFilled(accentMin, accentMax, IM_COL32(255, 255, 255, 210), 2.f);
        }

        ImFont* titleFont = UITheme::fontTitle ? UITheme::fontTitle : ImGui::GetFont();
        ImFont* smallFont = UITheme::fontSmall ? UITheme::fontSmall : ImGui::GetFont();

        if (!transparent)
        {
            const char* titleText = title ? title : "Cooldown Hub";
            dl->AddText(titleFont, 16.f,
                        { innerMin.x + 26.f, innerMin.y + 10.f },
                        IM_COL32(245, 247, 252, 255), titleText);

            char countText[32]{};
            if (godCount == 1)
                snprintf(countText, sizeof(countText), "1 god");
            else
                snprintf(countText, sizeof(countText), "%d gods", godCount);

            const ImVec2 countSize = smallFont->CalcTextSizeA(12.f, FLT_MAX, 0.f, countText);
            const ImVec2 countPos(innerMax.x - countSize.x - 14.f, innerMin.y + 13.f);
            dl->AddText(smallFont, 12.f, countPos,
                        IM_COL32(150, 156, 170, 255), countText);

            dl->AddLine(
                { innerMin.x + 12.f, innerMin.y + kTopChromeH - 6.f },
                { innerMax.x - 12.f, innerMin.y + kTopChromeH - 6.f },
                IM_COL32(255, 255, 255, 28), 1.f);
        }
        (void)titleFont;
        (void)smallFont;
        (void)title;
        (void)godCount;
    }

    static bool TryQualifyGodActor(SDK::AActor* actor,
                                   SDK::APawn* localPawn,
                                   SDK::AHWCharacter_Base** outCh,
                                   bool* outIsAlly)
    {
        if (!outCh || !outIsAlly)
            return false;

        *outCh = nullptr;
        *outIsAlly = false;

        __try {
            if (!actor || actor == localPawn || !Helpers::IsValidPtr(actor))
                return false;
            if (!Helpers::SafeIsA(actor, SDK::APawn::StaticClass()))
                return false;

            PawnFilter::EspCategory cat = PawnFilter::EspCategoryFor(
                actor, localPawn,
                cfg.ShowEnemies, cfg.ShowTeam, false, false, false);
            if (cat != PawnFilter::EspCategory::EnemyGod &&
                cat != PawnFilter::EspCategory::AllyGod)
                return false;
            if (!Helpers::SafeIsA(actor, SDK::AHWCharacter_Base::StaticClass()))
                return false;

            auto* ch = static_cast<SDK::AHWCharacter_Base*>(actor);
            if (!ch || !Helpers::IsValidPtr(ch) || Helpers::IsDead(ch))
                return false;

            *outCh = ch;
            *outIsAlly = (cat == PawnFilter::EspCategory::AllyGod);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        return false;
    }

    static void FillGodRow(GodRow& row,
                           SDK::AHWCharacter_Base* ch,
                           SDK::APawn* refPawn,
                           const SDK::FVector& localPos,
                           float worldTime,
                           bool isAlly)
    {
        row.sortKey = reinterpret_cast<uintptr_t>(ch);
        row.isAlly = isAlly;
        row.name = Helpers::GetPawnDisplayName(static_cast<SDK::APawn*>(ch));
        if (row.name.empty() || row.name == "HWPLAYERSTATE")
            row.name = "Unknown";
        row.distance = Helpers::Dist3D(Helpers::GetLocation(ch), localPos);
        row.godCode = AbilityIcons::ResolveGodCode(static_cast<SDK::APawn*>(ch));
        if ((row.name.empty() || row.name == "UNKNOWN" || row.name == "HWPLAYERSTATE") &&
            !row.godCode.empty())
        {
            row.name = row.godCode;
        }

        EspCooldownSeh::GetEnemyAbilityStates(
            static_cast<SDK::APawn*>(ch), worldTime, row.slots);

        char relicHint[48]{};
        EspCooldownSeh::GetEnemyRelicStates(
            static_cast<SDK::APawn*>(ch), worldTime, row.relicSlots, relicHint);

        if (relicHint[0])
            row.relicCode = relicHint;
        else
            row.relicCode = AbilityIcons::ResolveRelicCode(static_cast<SDK::APawn*>(ch));
        row.relicCode = AbilityIcons::CanonicalizeRelicCode(std::move(row.relicCode));

        if (!row.relicCode.empty() &&
            row.relicSlots[0].state == EspCooldownSeh::EspAbilitySlotState::Locked)
        {
            row.relicSlots[0].slot = 1;
            row.relicSlots[0].state = EspCooldownSeh::EspAbilitySlotState::Ready;
        }

        if (!row.godCode.empty())
        {
            for (int i = 0; i < EspCooldownSeh::kMaxCooldowns; ++i)
            {
                if (row.slots[i].state != EspCooldownSeh::EspAbilitySlotState::Locked)
                    continue;
                row.slots[i].slot = i + 1;
                row.slots[i].state = EspCooldownSeh::EspAbilitySlotState::Ready;
            }
        }

        (void)refPawn;
    }

    static bool TryFillGodRowSafe(GodRow& row,
                                  SDK::AHWCharacter_Base* ch,
                                  SDK::APawn* refPawn,
                                  const SDK::FVector& localPos,
                                  float worldTime,
                                  bool isAlly)
    {
        __try {
            if (!ch || !Helpers::IsValidPtr(ch))
                return false;
            FillGodRow(row, ch, refPawn, localPos, worldTime, isAlly);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    static void CollectGodRows(std::vector<GodRow>& rows,
                               SDK::APawn* localPawn,
                               const SDK::FVector& localPos,
                               float worldTime)
    {
        rows.clear();
        rows.reserve(kMaxWidgetSlots);

        if (!localPawn || (!cfg.ShowEnemies && !cfg.ShowTeam))
            return;

        int enemyCount = 0;
        int allyCount  = 0;

        if (cfg.ShowTeam && cfg.ShowSelf && localPawn &&
            Helpers::IsValidPtr(localPawn) &&
            Helpers::SafeIsA(localPawn, SDK::AHWCharacter_Base::StaticClass()))
        {
            auto* localCh = static_cast<SDK::AHWCharacter_Base*>(localPawn);
            if (!Helpers::IsDead(localCh))
            {
                GodRow selfRow;
                if (TryFillGodRowSafe(selfRow, localCh, localPawn, localPos, worldTime, true))
                {
                    rows.push_back(std::move(selfRow));
                    ++allyCount;
                }
            }
        }

        for (SDK::AActor* actor : Helpers::GetAllLevelActors())
        {
            if (enemyCount >= kMaxEnemyGods && allyCount >= kMaxTeamGods)
                break;

            SDK::AHWCharacter_Base* ch = nullptr;
            bool isAlly = false;
            if (!TryQualifyGodActor(actor, localPawn, &ch, &isAlly))
                continue;

            if (isAlly)
            {
                if (allyCount >= kMaxTeamGods)
                    continue;
                ++allyCount;
            }
            else
            {
                if (enemyCount >= kMaxEnemyGods)
                    continue;
                ++enemyCount;
            }

            GodRow row;
            if (!TryFillGodRowSafe(row, ch, localPawn, localPos, worldTime, isAlly))
                continue;
            rows.push_back(std::move(row));
        }

        std::sort(rows.begin(), rows.end(),
                  [](const GodRow& a, const GodRow& b) {
                      if (a.isAlly != b.isAlly)
                          return a.isAlly < b.isAlly;
                      return a.sortKey < b.sortKey;
                  });
    }

    static void PartitionRows(const std::vector<GodRow>& rows,
                              std::vector<GodRow>& enemies,
                              std::vector<GodRow>& allies)
    {
        enemies.clear();
        allies.clear();
        for (const GodRow& row : rows)
        {
            if (row.isAlly)
                allies.push_back(row);
            else
                enemies.push_back(row);
        }
    }

    static void ApplySavedPosOnce(float* savedPosX,
                                  float* savedPosY,
                                  ImVec2& seedPos,
                                  float fallbackX,
                                  float fallbackY)
    {
        const bool hasSavedPos = savedPosX && savedPosY &&
                                   *savedPosX >= 0.f && *savedPosY >= 0.f;
        if (hasSavedPos)
        {
            if (seedPos.x != *savedPosX || seedPos.y != *savedPosY)
            {
                ImGui::SetNextWindowPos(ImVec2(*savedPosX, *savedPosY), ImGuiCond_Always);
                seedPos = ImVec2(*savedPosX, *savedPosY);
            }
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(fallbackX, fallbackY), ImGuiCond_FirstUseEver);
        }
    }

    static void DrawHubDragHandle(const ImVec2& wpos, const ImVec2& wsize)
    {
        if (!Menu::WantsInput())
            return;

        ImGui::SetCursorScreenPos(wpos);
        ImGui::InvisibleButton("##cdhub_drag", ImVec2(wsize.x, kTopChromeH));
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))
        {
            const ImVec2 p = ImGui::GetWindowPos();
            ImGui::SetWindowPos(ImVec2(p.x + ImGui::GetIO().MouseDelta.x,
                                      p.y + ImGui::GetIO().MouseDelta.y));
        }
    }

    static void DrawGodRowWidget(const GodRow& row, int slotIndex)
    {
        if (slotIndex < 0 || slotIndex >= kMaxWidgetSlots)
            return;

        EnsureWidgetLayoutLoaded();

        const bool vertical = cfg.WidgetVerticalLayout;
        const AbilityCdUi::HubSlotFilter filter = BuildHubSlotFilter();
        const float tileSize = HubTileSizeClamped();
        const float nameSize = WidgetGodNameSizeClamped();
        const float rowH =
            AbilityCdUi::HubRowMetrics(tileSize, vertical, &filter, nameSize).rowHeight;
        const float hubWidth = MeasureHubWidth();
        const float winH = MeasureWidgetHeight();
        const float headerH =
            AbilityCdUi::HubRowMetrics(tileSize, vertical, &filter, nameSize).headerHeight;
        const bool allowResize = Menu::WantsInput();

        ImVec2& stored = s_widgetPos[slotIndex];
        if (stored.x < 0.f || stored.y < 0.f)
            stored = DefaultWidgetPos(slotIndex);

        ImVec2& seed = s_widgetSeed[slotIndex];
        if (seed.x != stored.x || seed.y != stored.y)
        {
            ImGui::SetNextWindowPos(stored, ImGuiCond_Always);
            seed = stored;
        }

        PushWindowStyle();
        if (allowResize)
        {
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(hubWidth * 0.55f, winH * 0.8f),
                ImVec2(900.f, 600.f));
            ImGui::SetNextWindowSize(ImVec2(hubWidth, winH), ImGuiCond_FirstUseEver);
        }
        else
        {
            ImGui::SetNextWindowSize(ImVec2(hubWidth, winH), ImGuiCond_Always);
        }

        char winId[32]{};
        snprintf(winId, sizeof(winId), "##cdw_slot_%d", slotIndex);

        if (!ImGui::Begin(winId, nullptr, HubWindowFlags(allowResize)))
        {
            PopWindowStyle();
            return;
        }

        const ImVec2 wpos = ImGui::GetWindowPos();
        const ImVec2 wsize = ImGui::GetWindowSize();
        const float padTop = ImGui::GetStyle().WindowPadding.y;
        const float contentW = wsize.x - kSidePad * 2.f;
        const ImVec2 rowMin(wpos.x + kSidePad, wpos.y + padTop);

        if (Menu::WantsInput())
        {
            ImGui::SetCursorScreenPos(rowMin);
            char dragId[32]{};
            snprintf(dragId, sizeof(dragId), "##cdw_drag_%d", slotIndex);
            ImGui::InvisibleButton(dragId, ImVec2(contentW, headerH + 2.f));
            if (ImGui::IsItemHovered())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))
            {
                const ImVec2 p = ImGui::GetWindowPos();
                ImGui::SetWindowPos(ImVec2(p.x + ImGui::GetIO().MouseDelta.x,
                                          p.y + ImGui::GetIO().MouseDelta.y));
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();

        const bool showRelic =
            !row.relicCode.empty() ||
            row.relicSlots[0].state != EspCooldownSeh::EspAbilitySlotState::Locked;
        const AbilityCdUi::HubDrawStyle rowStyle = BuildRowStyle(true, row.isAlly, vertical);
        AbilityCdUi::DrawHubGodRow(
            dl, rowMin, contentW,
            row.name.c_str(),
            row.distance / 100.f,
            row.godCode,
            row.slots,
            tileSize,
            showRelic ? &row.relicSlots[0] : nullptr,
            row.relicCode.empty() ? nullptr : &row.relicCode,
            &rowStyle);

        SyncHubTileSizeFromWindow(contentW, vertical, filter);

        ImGui::Dummy(ImVec2(contentW, rowH));

        stored = ImGui::GetWindowPos();
        seed = stored;

        ImGui::End();
        PopWindowStyle();
    }

    static void DrawTransparentWidgets(const std::vector<GodRow>& rows)
    {
        int enemyIdx = 0;
        int allyIdx  = 0;
        for (const GodRow& row : rows)
        {
            if (row.isAlly)
            {
                if (allyIdx >= kMaxTeamGods)
                    continue;
                DrawGodRowWidget(row, kMaxEnemyGods + allyIdx);
                ++allyIdx;
            }
            else
            {
                if (enemyIdx >= kMaxEnemyGods)
                    continue;
                DrawGodRowWidget(row, enemyIdx);
                ++enemyIdx;
            }
        }
    }

    static float DrawHubWindow(const char* windowId,
                               bool* windowOpen,
                               const char* title,
                               const std::vector<GodRow>& rows,
                               const char* emptyMsg,
                               float defaultPosX,
                               float defaultPosY,
                               float* savedPosX,
                               float* savedPosY,
                               ImVec2& seedPos,
                               bool allyTint,
                               bool useSectionDividers)
    {
        PushWindowStyle();

        ImGuiIO& io = ImGui::GetIO();
        const int godCount = (int)rows.size();
        const bool showEmpty = rows.empty() && emptyMsg && emptyMsg[0];
        const int dividers = useSectionDividers ? CountSectionDividers(rows) : 0;
        const float winH = MeasureHubHeight(godCount, showEmpty, dividers);
        const float hubWidth = MeasureHubWidth();
        const float fallbackX = defaultPosX >= 0.f
            ? defaultPosX
            : (io.DisplaySize.x - hubWidth - kHubRightPad);
        const float fallbackY = defaultPosY >= 0.f ? defaultPosY : kHubTopY;

        const bool allowResize = Menu::WantsInput();
        const AbilityCdUi::HubSlotFilter filter = BuildHubSlotFilter();
        if (allowResize)
        {
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(hubWidth * 0.55f, 120.f),
                ImVec2(1200.f, 900.f));
            ImGui::SetNextWindowSize(ImVec2(hubWidth, winH), ImGuiCond_FirstUseEver);
        }
        else
        {
            ImGui::SetNextWindowSize(ImVec2(hubWidth, winH), ImGuiCond_Always);
        }
        ApplySavedPosOnce(savedPosX, savedPosY, seedPos, fallbackX, fallbackY);

        if (!ImGui::Begin(windowId, windowOpen, HubWindowFlags(allowResize)))
        {
            PopWindowStyle();
            return winH;
        }

        const ImVec2 wpos = ImGui::GetWindowPos();
        const ImVec2 wsize = ImGui::GetWindowSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        DrawHubDragHandle(wpos, wsize);
        DrawHubChrome(dl, wpos, wsize, godCount, title, cfg.TransparentBackground);

        if (savedPosX && savedPosY)
        {
            const ImVec2 livePos = ImGui::GetWindowPos();
            *savedPosX = livePos.x;
            *savedPosY = livePos.y;
            seedPos = livePos;
        }

        const float contentW = ImGui::GetContentRegionAvail().x;
        const float headerSpacerH = kTopChromeH - ImGui::GetCursorPosY();
        if (headerSpacerH > 0.f)
            ImGui::Dummy(ImVec2(contentW, headerSpacerH));

        const float tileSize = HubTileSizeClamped();
        SyncHubTileSizeFromWindow(contentW, false, filter);

        if (!rows.empty())
        {
            const float rowH =
                AbilityCdUi::HubRowMetrics(tileSize, false, &filter).rowHeight;

            for (int i = 0; i < godCount; ++i)
            {
                if (useSectionDividers &&
                    rows[i].isAlly && (i == 0 || !rows[i - 1].isAlly))
                {
                    ImGui::Dummy(ImVec2(contentW, kSectionDividerH));
                    const ImVec2 divMin = ImGui::GetItemRectMin();
                    if (!cfg.TransparentBackground)
                        DrawSectionDivider(dl, divMin, contentW, "Team");
                }

                ImGui::Dummy(ImVec2(contentW, rowH));
                const ImVec2 rowMin = ImGui::GetItemRectMin();
                const bool tintRow = !cfg.TransparentBackground && (allyTint || rows[i].isAlly);
                if (tintRow)
                {
                    dl->AddRectFilled(
                        { rowMin.x - 2.f, rowMin.y },
                        { rowMin.x + contentW + 2.f, rowMin.y + rowH },
                        IM_COL32(60, 130, 220, 14), 6.f);
                }
                const bool showRelic =
                    !rows[i].relicCode.empty() ||
                    rows[i].relicSlots[0].state != EspCooldownSeh::EspAbilitySlotState::Locked;
                const AbilityCdUi::HubDrawStyle rowStyle =
                    BuildRowStyle(false, rows[i].isAlly, false);
                AbilityCdUi::DrawHubGodRow(
                    dl, rowMin, contentW,
                    rows[i].name.c_str(),
                    rows[i].distance / 100.f,
                    rows[i].godCode,
                    rows[i].slots,
                    tileSize,
                    showRelic ? &rows[i].relicSlots[0] : nullptr,
                    rows[i].relicCode.empty() ? nullptr : &rows[i].relicCode,
                    &rowStyle);
            }

            ImGui::Dummy(ImVec2(contentW, kBottomPad));
        }
        else if (showEmpty)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.57f, 0.62f, 1.f));
            ImGui::TextUnformatted(emptyMsg);
            ImGui::PopStyleColor();
        }

        ImGui::End();
        PopWindowStyle();
        return winH;
    }

    static void DrawCombinedHub(const char* emptyMsg,
                                const std::vector<GodRow>& rows)
    {
        DrawHubWindow("##hw_cd_hub", &s_combinedHubOpen, "Cooldown Hub", rows, emptyMsg,
                      -1.f, kHubTopY,
                      &cfg.CombinedPosX, &cfg.CombinedPosY, s_seedCombinedPos,
                      false, true);
    }

    static void DrawSplitHubs(const char* enemyEmptyMsg,
                              const char* teamEmptyMsg,
                              const std::vector<GodRow>& rows)
    {
        std::vector<GodRow> enemies;
        std::vector<GodRow> allies;
        PartitionRows(rows, enemies, allies);

        float teamDefaultY = kHubTopY;
        if (cfg.ShowEnemies)
        {
            const float enemyDefaultY = cfg.EnemyHubPosY >= 0.f ? cfg.EnemyHubPosY : kHubTopY;
            const float enemyH = DrawHubWindow(
                "##hw_cd_hub_enemies", &s_enemyHubOpen, "Enemy Cooldowns",
                enemies, enemyEmptyMsg,
                -1.f, enemyDefaultY,
                &cfg.EnemyHubPosX, &cfg.EnemyHubPosY, s_seedEnemyPos,
                false, false);
            teamDefaultY = cfg.EnemyHubPosY + enemyH + kHubStackGap;
        }

        if (cfg.ShowTeam)
        {
            const float teamDefaultYPos = cfg.TeamHubPosY >= 0.f ? cfg.TeamHubPosY : teamDefaultY;
            DrawHubWindow(
                "##hw_cd_hub_team", &s_teamHubOpen, "Team Cooldowns",
                allies, teamEmptyMsg,
                -1.f, teamDefaultYPos,
                &cfg.TeamHubPosX, &cfg.TeamHubPosY, s_seedTeamPos,
                true, false);
        }
    }

    void Tick()
    {
        if (!cfg.Enabled)
            return;

        if (!GameState::IsPlayable())
        {
            if (cfg.WidgetMode)
                return;
            if (cfg.SplitHubs)
                DrawSplitHubs("Waiting for match...", "Waiting for match...", {});
            else
                DrawCombinedHub("Waiting for match...", {});
            return;
        }

        auto* pc = Helpers::GetLocalPC();
        if (!pc)
        {
            if (cfg.WidgetMode)
                return;
            if (cfg.SplitHubs)
                DrawSplitHubs("Waiting for match...", "Waiting for match...", {});
            else
                DrawCombinedHub("Waiting for match...", {});
            return;
        }

        SDK::APawn* localPawn = Helpers::SafeGetPawnFromPC(pc);
        bool localDead = false;

        if (localPawn && Helpers::IsValidPtr(localPawn) &&
            Helpers::SafeIsA(localPawn, SDK::AHWCharacter_Base::StaticClass()))
        {
            localDead = Helpers::IsDead(static_cast<SDK::AHWCharacter_Base*>(localPawn));
        }

        const float worldTime = Helpers::GetWorldTimeSeconds();
        const SDK::FVector localPos = localPawn ? Helpers::GetLocation(localPawn) : SDK::FVector{};

        std::vector<GodRow> rows;
        CollectGodRows(rows, localPawn, localPos, worldTime);

        if (cfg.WidgetMode)
        {
            if (!rows.empty())
                DrawTransparentWidgets(rows);
            return;
        }

        if (cfg.SplitHubs)
        {
            const char* enemyEmpty = nullptr;
            const char* teamEmpty  = nullptr;

            if (localDead)
            {
                enemyEmpty = "No living enemy gods";
                teamEmpty  = "No living allied gods";
            }
            else if (!cfg.ShowEnemies && !cfg.ShowTeam)
            {
                enemyEmpty = "Enable Show Enemies or Show Team";
                teamEmpty  = enemyEmpty;
            }
            else
            {
                if (cfg.ShowEnemies)
                    enemyEmpty = "No enemy gods alive";
                if (cfg.ShowTeam)
                    teamEmpty = "No allied gods alive";
            }

            DrawSplitHubs(enemyEmpty, teamEmpty, rows);
            return;
        }

        if (localDead)
            DrawCombinedHub(rows.empty() ? "No living gods tracked" : nullptr, rows);
        else if (rows.empty())
            DrawCombinedHub(!cfg.ShowEnemies && !cfg.ShowTeam
                                ? "Enable Show Enemies or Show Team"
                                : "No tracked gods alive",
                            rows);
        else
            DrawCombinedHub(nullptr, rows);
    }
}
