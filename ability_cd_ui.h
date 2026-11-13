#pragma once
#include "ability_icons.h"
#include "esp_cooldown_seh.h"
#include "../imgui/imgui.h"

namespace AbilityCdUi
{
    struct HubSlotFilter
    {
        bool showName = true;
        bool showDistance = true;
        bool showAbility[4] = { true, true, true, true };
        bool showRelic = true;
    };

    struct HubDrawStyle
    {
        bool transparentBackground = false;
        bool isAlly = false;
        bool verticalLayout = false;
        float godNameFontSize = 15.f;
        const HubSlotFilter* slotFilter = nullptr;
    };

    struct SlotVisual
    {
        float progressFill = 0.f;
        ImU32 borderCol    = IM_COL32(255, 255, 255, 120);
        ImU32 glowCol      = IM_COL32(0, 0, 0, 0);
        ImU32 progressCol  = IM_COL32(255, 170, 60, 110);
        ImU32 labelCol     = IM_COL32(255, 255, 255, 255);
        char  label[16]{};
        bool  showLabel    = false;
        bool  showProgress = false;
        bool  dimIcon      = false;
    };

    struct HubRowLayout
    {
        float rowHeight   = 0.f;
        float tileSize    = 0.f;
        float headerHeight = 0.f;
    };

    SlotVisual BuildSlotVisual(const EspCooldownSeh::EspAbilitySlotInfo& slot);

    HubRowLayout HubRowMetrics(float tileSize = 38.f,
                               bool vertical = false,
                               const HubSlotFilter* filter = nullptr,
                               float godNameFontSize = 15.f);

    void DrawAbilityTile(ImDrawList* dl,
                         const ImVec2& topLeft,
                         float size,
                         const AbilityIcons::IconView& icon,
                         const EspCooldownSeh::EspAbilitySlotInfo& slot,
                         int slotIndex,
                         bool showSlotBadge = true,
                         bool relicBadge = false,
                         const HubDrawStyle* style = nullptr);

    float DrawHubGodRow(ImDrawList* dl,
                        const ImVec2& rowMin,
                        float rowWidth,
                        const char* godName,
                        float distanceMeters,
                        const std::string& godCode,
                        const EspCooldownSeh::EspAbilitySlotInfo slots[EspCooldownSeh::kMaxCooldowns],
                        float tileSize = 38.f,
                        const EspCooldownSeh::EspAbilitySlotInfo* relicSlot = nullptr,
                        const std::string* relicCode = nullptr,
                        const HubDrawStyle* style = nullptr);

    float MeasureHubRowWidth(float tileSize = 38.f,
                             bool vertical = false,
                             const HubSlotFilter* filter = nullptr);

    void DrawEspPanel(ImDrawList* dl,
                      const ImVec2& topLeft,
                      float tileSize,
                      float gap,
                      float pad,
                      const AbilityIcons::IconView icons[EspCooldownSeh::kMaxCooldowns],
                      const EspCooldownSeh::EspAbilitySlotInfo slots[EspCooldownSeh::kMaxCooldowns],
                      const AbilityIcons::IconView* relicIcon = nullptr,
                      const EspCooldownSeh::EspAbilitySlotInfo* relicSlot = nullptr);
}
