#include "ability_cd_ui.h"
#include "../ui/theme.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace AbilityCdUi
{
    static void DrawCenteredLabel(ImDrawList* dl,
                                  ImFont* font,
                                  float fontSize,
                                  const ImVec2& center,
                                  const char* text,
                                  ImU32 col,
                                  ImU32 shadowCol = IM_COL32(0, 0, 0, 200))
    {
        if (!text || !text[0])
            return;

        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, text);
        const ImVec2 pos(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f);
        dl->AddText(font, fontSize, { pos.x + 1.f, pos.y + 1.f }, shadowCol, text);
        dl->AddText(font, fontSize, pos, col, text);
    }

    static void DrawSlotBadge(ImDrawList* dl,
                              const ImVec2& topLeft,
                              float size,
                              int slotIndex,
                              bool relicBadge)
    {
        const char* badge = nullptr;
        char numBadge[4]{};
        if (relicBadge)
            badge = "R";
        else if (slotIndex >= 1 && slotIndex <= 4)
        {
            snprintf(numBadge, sizeof(numBadge), "%d", slotIndex);
            badge = numBadge;
        }
        else
            return;

        ImFont* font = UITheme::fontSmall ? UITheme::fontSmall : ImGui::GetFont();
        const float fontSize = size * 0.17f;
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, badge);

        const float padX = size * 0.06f;
        const float padY = size * 0.05f;
        const ImVec2 badgeMin(topLeft.x + padX, topLeft.y + padY);
        const ImVec2 badgeMax(badgeMin.x + textSize.x + padX * 1.1f,
                              badgeMin.y + textSize.y + padY * 0.7f);

        dl->AddRectFilled(badgeMin, badgeMax, IM_COL32(0, 0, 0, 140), 3.f);
        dl->AddText(font, fontSize,
                    { badgeMin.x + padX * 0.55f, badgeMin.y + padY * 0.15f },
                    IM_COL32(235, 238, 245, 240), badge);
    }

    SlotVisual BuildSlotVisual(const EspCooldownSeh::EspAbilitySlotInfo& slot)
    {
        SlotVisual v{};

        if (slot.state == EspCooldownSeh::EspAbilitySlotState::Locked)
        {
            v.progressFill = 0.f;
            v.borderCol    = IM_COL32(90, 94, 108, 160);
            v.dimIcon      = true;
            v.showLabel    = false;
            v.showProgress = false;
            return v;
        }

        if (slot.state == EspCooldownSeh::EspAbilitySlotState::OnCooldown)
        {
            const float dur = slot.duration > 0.05f ? slot.duration : slot.remaining;
            v.progressFill = (dur > 0.f) ? (1.f - (slot.remaining / dur)) : 0.f;
            if (v.progressFill < 0.f) v.progressFill = 0.f;
            if (v.progressFill > 1.f) v.progressFill = 1.f;

            v.borderCol    = IM_COL32(255, 48, 48, 255);
            v.glowCol      = IM_COL32(255, 60, 60, 70);
            v.progressCol  = IM_COL32(255, 90, 90, 130);
            v.labelCol     = IM_COL32(255, 245, 245, 255);
            v.dimIcon      = true;
            v.showProgress = true;
            v.showLabel    = true;

            if (slot.remaining >= 10.f)
                snprintf(v.label, sizeof(v.label), "%.0f", slot.remaining);
            else if (slot.remaining > 0.05f)
                snprintf(v.label, sizeof(v.label), "%.1f", slot.remaining);
            else
                v.label[0] = '\0';
            return v;
        }

        v.progressFill = 1.f;
        v.borderCol    = IM_COL32(72, 220, 128, 220);
        v.glowCol      = IM_COL32(60, 200, 110, 40);
        v.labelCol     = IM_COL32(220, 255, 230, 255);
        v.showProgress = false;
        v.showLabel    = true;
        snprintf(v.label, sizeof(v.label), "READY");
        return v;
    }

    static int CountVisibleAbilitySlots(const HubSlotFilter* filter)
    {
        int count = 0;
        for (int i = 0; i < EspCooldownSeh::kMaxCooldowns; ++i)
        {
            if (!filter || filter->showAbility[i])
                ++count;
        }
        return count;
    }

    HubRowLayout HubRowMetrics(float tileSize, bool vertical, const HubSlotFilter* filter,
                               float godNameFontSize)
    {
        HubRowLayout m{};
        m.tileSize = tileSize;
        const bool showName = !filter || filter->showName;
        m.headerHeight = showName ? (godNameFontSize + 4.f) : 8.f;

        constexpr float kTileGap = 4.f;
        int tileRows = CountVisibleAbilitySlots(filter);
        if (!filter || filter->showRelic)
            tileRows += (tileRows > 0 || !filter) ? 1 : 0;
        if (tileRows <= 0)
            tileRows = 1;

        if (!vertical)
        {
            m.rowHeight = m.headerHeight + 3.f + tileSize + 6.f;
            return m;
        }

        m.rowHeight = m.headerHeight + 3.f +
                      tileRows * tileSize +
                      (tileRows - 1) * kTileGap + 6.f;
        return m;
    }

    void DrawAbilityTile(ImDrawList* dl,
                         const ImVec2& topLeft,
                         float size,
                         const AbilityIcons::IconView& icon,
                         const EspCooldownSeh::EspAbilitySlotInfo& slot,
                         int slotIndex,
                         bool showSlotBadge,
                         bool relicBadge,
                         const HubDrawStyle* style)
    {
        const bool transparent = style && style->transparentBackground;
        const SlotVisual vis = BuildSlotVisual(slot);
        const ImVec2 botRight(topLeft.x + size, topLeft.y + size);
        const ImVec2 center(topLeft.x + size * 0.5f, topLeft.y + size * 0.5f);
        const float rounding = size * 0.16f;
        const bool onCooldown = (slot.state == EspCooldownSeh::EspAbilitySlotState::OnCooldown);

        const bool ready = (slot.state == EspCooldownSeh::EspAbilitySlotState::Ready);
        if (vis.glowCol & 0xFF000000)
        {
            if (ready || !transparent)
            {
                dl->AddRectFilled(
                    { topLeft.x - 1.f, topLeft.y - 1.f },
                    { botRight.x + 1.f, botRight.y + 1.f },
                    vis.glowCol, rounding + 1.f);
            }
        }

        if (!transparent)
            dl->AddRectFilled(topLeft, botRight, IM_COL32(18, 20, 28, 200), rounding);

        const float pad = size * 0.05f;
        const ImVec2 imgMin(topLeft.x + pad, topLeft.y + pad);
        const ImVec2 imgMax(botRight.x - pad, botRight.y - pad);

        if (icon.valid && icon.texId != ImTextureID_Invalid)
        {
            const ImU32 iconTint = onCooldown
                ? IM_COL32(120, 125, 140, 150)
                : (vis.dimIcon ? IM_COL32(210, 210, 220, 170)
                               : IM_COL32(255, 255, 255, 255));
            dl->AddImageRounded(icon.texId, imgMin, imgMax,
                                { 0.f, 0.f }, { 1.f, 1.f },
                                iconTint, rounding * 0.55f);
        }
        else if (!transparent)
        {
            dl->AddRectFilled(imgMin, imgMax, IM_COL32(40, 44, 56, 240), rounding * 0.55f);
        }

        if (vis.dimIcon && !transparent)
            dl->AddRectFilled(imgMin, imgMax, IM_COL32(0, 0, 0, 75), rounding * 0.55f);

        if (onCooldown && vis.showProgress && vis.progressFill > 0.001f)
        {
            const float innerH = imgMax.y - imgMin.y;
            const float fillH = innerH * vis.progressFill;
            const ImVec2 fillMin(imgMin.x, imgMax.y - fillH);
            const ImVec2 fillMax(imgMax.x, imgMax.y);

            dl->AddRectFilled(fillMin, fillMax, vis.progressCol, rounding * 0.45f);

            if (vis.progressFill < 0.995f && !transparent)
            {
                const float veilH = innerH * (1.f - vis.progressFill);
                dl->AddRectFilled(
                    imgMin,
                    { imgMax.x, imgMin.y + veilH },
                    IM_COL32(0, 0, 0, 70),
                    rounding * 0.55f,
                    ImDrawFlags_RoundCornersTop);

                dl->AddLine(
                    { imgMin.x, fillMin.y },
                    { imgMax.x, fillMin.y },
                    IM_COL32(255, 170, 170, 170), 1.2f);
            }
        }

        if (onCooldown)
        {
            const ImU32 redFill = transparent
                ? IM_COL32(255, 55, 55, 38)
                : IM_COL32(255, 55, 55, 28);
            dl->AddRectFilled(imgMin, imgMax, redFill, rounding * 0.55f);

            const float stripeStep = size * 0.22f;
            const ImU32 stripeCol = IM_COL32(0, 0, 0, transparent ? 55 : 75);
            for (float d = -size; d < size * 2.f; d += stripeStep)
            {
                dl->AddLine(
                    { topLeft.x + d, topLeft.y },
                    { topLeft.x + d + size, topLeft.y + size },
                    stripeCol, 1.4f);
            }

            dl->AddRect(topLeft, botRight, IM_COL32(255, 70, 70, transparent ? 160 : 210),
                        rounding, 0, transparent ? 2.f : 2.4f);
        }

        if (ready)
        {
            const ImU32 greenFill = transparent
                ? IM_COL32(70, 220, 120, 28)
                : IM_COL32(70, 220, 120, 18);
            dl->AddRectFilled(imgMin, imgMax, greenFill, rounding * 0.55f);
        }

        if (!transparent || onCooldown || ready)
            dl->AddRect(topLeft, botRight, vis.borderCol, rounding, 0, transparent ? 1.2f : 1.6f);

        if (vis.showLabel && vis.label[0])
        {
            ImFont* font = UITheme::fontTitle ? UITheme::fontTitle : ImGui::GetFont();
            const bool readyLabel = (slot.state == EspCooldownSeh::EspAbilitySlotState::Ready);
            const float fontSize = readyLabel ? size * 0.17f : size * 0.32f;
            DrawCenteredLabel(dl, font, fontSize, center, vis.label, vis.labelCol);
        }

        if (showSlotBadge)
            DrawSlotBadge(dl, topLeft, size, slotIndex, relicBadge);
    }

    float MeasureHubRowWidth(float tileSize, bool vertical, const HubSlotFilter* filter)
    {
        constexpr float kTileGap = 4.f;
        constexpr float kRelicGap = 3.f;
        constexpr float kDistAfterTiles = 6.f;
        constexpr float kDistPillW = 48.f;

        const int abilityCount = CountVisibleAbilitySlots(filter);
        const bool showRelic = !filter || filter->showRelic;
        const bool showDistance = !filter || filter->showDistance;

        float width = tileSize;
        if (abilityCount > 0)
            width = abilityCount * (tileSize + kTileGap) - kTileGap;
        if (showRelic && abilityCount > 0)
            width += kRelicGap + tileSize;
        else if (showRelic)
            width = tileSize;

        if (showDistance)
            width += kDistAfterTiles + kDistPillW;

        if (vertical)
        {
            const float minVerticalW = tileSize + (showDistance ? kDistPillW : 0.f);
            if (width < minVerticalW)
                width = minVerticalW;
        }

        return width + 4.f;
    }

    float DrawHubGodRow(ImDrawList* dl,
                        const ImVec2& rowMin,
                        float rowWidth,
                        const char* godName,
                        float distanceMeters,
                        const std::string& godCode,
                        const EspCooldownSeh::EspAbilitySlotInfo slots[EspCooldownSeh::kMaxCooldowns],
                        float tileSize,
                        const EspCooldownSeh::EspAbilitySlotInfo* relicSlot,
                        const std::string* relicCode,
                        const HubDrawStyle* style)
    {
        const bool transparent = style && style->transparentBackground;
        const bool isAlly = style && style->isAlly;
        const bool vertical = style && style->verticalLayout;
        const HubSlotFilter* filter = style ? style->slotFilter : nullptr;
        const bool showName = !filter || filter->showName;
        const bool showDistance = !filter || filter->showDistance;
        const bool allowRelic = !filter || filter->showRelic;
        const float nameFontSize = style && style->godNameFontSize > 0.f
            ? style->godNameFontSize
            : 15.f;
        const HubRowLayout metrics = HubRowMetrics(tileSize, vertical, filter, nameFontSize);
        const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + metrics.rowHeight);

        ImFont* nameFont = UITheme::fontTitle ? UITheme::fontTitle : ImGui::GetFont();
        ImFont* smallFont = UITheme::fontSmall ? UITheme::fontSmall : ImGui::GetFont();

        const char* displayName = (godName && godName[0]) ? godName : "Unknown";
        if (showName)
        {
            const ImU32 nameCol = isAlly
                ? IM_COL32(72, 220, 128, 255)
                : IM_COL32(255, 88, 88, 255);
            if (transparent)
            {
                dl->AddText(nameFont, nameFontSize, { rowMin.x + 1.f, rowMin.y + 1.f },
                            IM_COL32(0, 0, 0, 180), displayName);
            }
            dl->AddText(nameFont, nameFontSize, rowMin, nameCol, displayName);
        }

        char distText[16]{};
        snprintf(distText, sizeof(distText), "%.0fm", distanceMeters);
        const ImVec2 distSize = smallFont->CalcTextSizeA(12.f, FLT_MAX, 0.f, distText);

        const float tileGap = 4.f;
        const float tilesY = rowMin.y + metrics.headerHeight + 3.f;
        float tilesEndX = rowMin.x;
        float tilesEndY = tilesY;

        const bool drawRelicData = allowRelic && relicSlot &&
            (relicSlot->state != EspCooldownSeh::EspAbilitySlotState::Locked ||
             (relicCode && !relicCode->empty()));

        if (vertical)
        {
            if (showDistance)
            {
                const float distY = rowMin.y + (metrics.headerHeight - distSize.y) * 0.5f;
                const ImVec2 pillMin(rowMin.x + rowWidth - distSize.x - 10.f, distY - 1.f);
                const ImVec2 pillMax(pillMin.x + distSize.x + 10.f, distY + distSize.y + 1.f);
                if (!transparent)
                {
                    dl->AddRectFilled(pillMin, pillMax, IM_COL32(255, 255, 255, 18), 6.f);
                    dl->AddRect(pillMin, pillMax, IM_COL32(255, 255, 255, 40), 6.f, 0, 1.f);
                }
                if (transparent)
                {
                    dl->AddText(smallFont, 12.f,
                                { pillMin.x + 5.f + 1.f, distY + 1.f },
                                IM_COL32(0, 0, 0, 180), distText);
                }
                dl->AddText(smallFont, 12.f,
                            { pillMin.x + 5.f, distY },
                            IM_COL32(170, 176, 190, 255), distText);
            }

            float tileY = tilesY;
            for (int i = 0; i < EspCooldownSeh::kMaxCooldowns; ++i)
            {
                if (filter && !filter->showAbility[i])
                    continue;

                const int slotIndex = slots[i].slot > 0 ? slots[i].slot : (i + 1);
                const AbilityIcons::IconView icon =
                    AbilityIcons::GetAbilityIcon(godCode, slotIndex);
                const ImVec2 pos(rowMin.x, tileY);
                DrawAbilityTile(dl, pos, tileSize, icon, slots[i], slotIndex, true, false, style);
                tileY += tileSize + tileGap;
                tilesEndY = pos.y + tileSize;
                tilesEndX = pos.x + tileSize;
            }

            if (drawRelicData)
            {
                const std::string code = relicCode ? *relicCode : std::string{};
                const AbilityIcons::IconView relicIcon = AbilityIcons::GetRelicIcon(code);
                const ImVec2 pos(rowMin.x, tileY);
                DrawAbilityTile(dl, pos, tileSize, relicIcon, *relicSlot, 0, true, true, style);
                tilesEndY = pos.y + tileSize;
                tilesEndX = pos.x + tileSize;
            }
        }
        else
        {
            float tilesX = rowMin.x;

            for (int i = 0; i < EspCooldownSeh::kMaxCooldowns; ++i)
            {
                if (filter && !filter->showAbility[i])
                    continue;

                const int slotIndex = slots[i].slot > 0 ? slots[i].slot : (i + 1);
                const AbilityIcons::IconView icon =
                    AbilityIcons::GetAbilityIcon(godCode, slotIndex);
                const ImVec2 pos(tilesX, tilesY);
                DrawAbilityTile(dl, pos, tileSize, icon, slots[i], slotIndex, true, false, style);
                tilesX = pos.x + tileSize + tileGap;
                tilesEndX = pos.x + tileSize;
            }

            if (drawRelicData)
            {
                const std::string code = relicCode ? *relicCode : std::string{};
                const AbilityIcons::IconView relicIcon = AbilityIcons::GetRelicIcon(code);
                const float relicX = (tilesEndX > rowMin.x) ? tilesEndX + 3.f : rowMin.x;
                const ImVec2 pos(relicX, tilesY);
                DrawAbilityTile(dl, pos, tileSize, relicIcon, *relicSlot, 0, true, true, style);
                tilesEndX = pos.x + tileSize;
            }

            if (showDistance)
            {
                const float distY = tilesY + (tileSize - distSize.y) * 0.5f;
                const ImVec2 pillMin(tilesEndX + 6.f, distY - 1.f);
                const ImVec2 pillMax(pillMin.x + distSize.x + 10.f, distY + distSize.y + 1.f);
                if (!transparent)
                {
                    dl->AddRectFilled(pillMin, pillMax, IM_COL32(255, 255, 255, 18), 6.f);
                    dl->AddRect(pillMin, pillMax, IM_COL32(255, 255, 255, 40), 6.f, 0, 1.f);
                }
                if (transparent)
                {
                    dl->AddText(smallFont, 12.f,
                                { pillMin.x + 5.f + 1.f, distY + 1.f },
                                IM_COL32(0, 0, 0, 180), distText);
                }
                dl->AddText(smallFont, 12.f,
                            { pillMin.x + 5.f, distY },
                            IM_COL32(170, 176, 190, 255), distText);
            }
        }

        (void)tilesEndX;
        (void)tilesEndY;

        if (!transparent)
        {
            dl->AddLine(
                { rowMin.x, rowMax.y - 1.f },
                { rowMax.x, rowMax.y - 1.f },
                IM_COL32(255, 255, 255, 22), 1.f);
        }

        return metrics.rowHeight;
    }

    void DrawEspPanel(ImDrawList* dl,
                      const ImVec2& topLeft,
                      float tileSize,
                      float gap,
                      float pad,
                      const AbilityIcons::IconView icons[EspCooldownSeh::kMaxCooldowns],
                      const EspCooldownSeh::EspAbilitySlotInfo slots[EspCooldownSeh::kMaxCooldowns],
                      const AbilityIcons::IconView* relicIcon,
                      const EspCooldownSeh::EspAbilitySlotInfo* relicSlot)
    {
        const bool showRelic = relicSlot &&
            (relicSlot->state != EspCooldownSeh::EspAbilitySlotState::Locked ||
             (relicIcon && relicIcon->valid));
        const float relicRowH = showRelic ? (tileSize + gap) : 0.f;
        const float panelW = tileSize * 2.f + gap + pad * 2.f;
        const float panelH = tileSize * 2.f + gap + pad * 2.f + relicRowH;
        const ImVec2 panelMin(topLeft.x - pad, topLeft.y - pad);
        const ImVec2 panelMax(topLeft.x + panelW - pad, topLeft.y + panelH - pad);

        dl->AddRectFilled(
            { panelMin.x + 2.f, panelMin.y + 3.f },
            { panelMax.x + 2.f, panelMax.y + 3.f },
            IM_COL32(0, 0, 0, 70), 10.f);
        dl->AddRectFilled(panelMin, panelMax, IM_COL32(6, 8, 14, 215), 10.f);
        dl->AddRect(panelMin, panelMax, IM_COL32(255, 255, 255, 55), 10.f, 0, 1.5f);

        for (int i = 0; i < EspCooldownSeh::kMaxCooldowns; ++i)
        {
            const int col = i % 2;
            const int row = i / 2;
            const ImVec2 pos(
                topLeft.x + col * (tileSize + gap),
                topLeft.y + row * (tileSize + gap));
            const int slotIndex = slots[i].slot > 0 ? slots[i].slot : (i + 1);
            DrawAbilityTile(dl, pos, tileSize, icons[i], slots[i], slotIndex, true, false);
        }

        if (showRelic && relicIcon)
        {
            const ImVec2 pos(
                topLeft.x + (panelW - pad * 2.f - tileSize) * 0.5f,
                topLeft.y + 2.f * (tileSize + gap));
            DrawAbilityTile(dl, pos, tileSize, *relicIcon, *relicSlot, 0, true, true);
        }
    }
}
