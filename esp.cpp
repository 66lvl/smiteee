#include "esp.h"
#include "esp_cooldown_seh.h"
#include "ability_icons.h"
#include "ability_cd_ui.h"
#include "../core/sdk_helpers.h"
#include "../core/game_state.h"
#include "../core/pawn_filter.h"
#include "../core/bones.h"
#include "../ui/theme.h"
#include "../imgui/imgui.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace ESP
{
    Config cfg;

    // Cache value snapshots only. No UObject pointers, roots or flags are
    // retained here; each drawable actor still comes from this frame's level list.
    struct CachedData
    {
        int index = -1;
        SDK::FName name{};
        uintptr_t type = 0;
        double lastSeen = 0.0;
        double categoryAt = -1.0, nameAt = -1.0, codesAt = -1.0, cooldownAt = -1.0;
        PawnFilter::EspCategory category = PawnFilter::EspCategory::Skip;
        std::string playerName, godCode, relicCode;
        float sampleWorldTime = 0.f;
        double iconRetryAt = -1.0;
        bool hasAbilityIcon[EspCooldownSeh::kMaxCooldowns]{};
        bool hasRelicIcon = false;
        EspCooldownSeh::EspAbilitySlotInfo slots[EspCooldownSeh::kMaxCooldowns]{};
        EspCooldownSeh::EspAbilitySlotInfo relic[EspCooldownSeh::kMaxRelicSlots]{};
    };
    static std::unordered_map<uintptr_t, CachedData> s_data;
    // Capacity is reused; contents are cleared before/after every frame.
    static std::vector<SDK::AActor*> s_frameActors;
    static uintptr_t s_world = 0, s_level = 0, s_localPawn = 0;
    static unsigned s_filterMask = 0;
    static double s_pruneAt = 0.0;

    void Reset()
    {
        s_frameActors.clear();
        s_data.clear();
        s_world = s_level = s_localPawn = 0;
        s_filterMask = 0;
        s_pruneAt = 0.0;
    }

    static bool ReadWorldStamp(uintptr_t& worldId, uintptr_t& levelId)
    {
        __try {
            auto* world = Helpers::GetWorld();
            if (!world || !Helpers::IsValidPtr(world)) return false;
            auto* level = world->PersistentLevel;
            if (!level || !Helpers::IsValidPtr(level)) return false;
            worldId = reinterpret_cast<uintptr_t>(world);
            levelId = reinterpret_cast<uintptr_t>(level);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    static CachedData& DataFor(SDK::AActor* actor, double now)
    {
        const uintptr_t key = reinterpret_cast<uintptr_t>(actor);
        // Bound memory even when many short-lived pawns are encountered.
        if (s_data.size() >= 4096 && s_data.find(key) == s_data.end()) s_data.clear();
        auto& data = s_data[key];
        const uintptr_t type = reinterpret_cast<uintptr_t>(actor->Class);
        if (data.index != actor->Index || data.name != actor->Name || data.type != type)
        {
            data = CachedData{};
            data.index = actor->Index; data.name = actor->Name; data.type = type;
        }
        data.lastSeen = now;
        return data;
    }

    static void AdvanceCooldown(EspCooldownSeh::EspAbilitySlotInfo& slot, float elapsed)
    {
        if (slot.state != EspCooldownSeh::EspAbilitySlotState::OnCooldown) return;
        slot.remaining = fmaxf(0.f, slot.remaining - elapsed);
        if (slot.remaining <= 0.f) slot.state = EspCooldownSeh::EspAbilitySlotState::Ready;
    }

    static float ScreenW() { return ImGui::GetIO().DisplaySize.x; }
    static float ScreenH() { return ImGui::GetIO().DisplaySize.y; }

    static bool CapsuleBBoxScreen(SDK::FVector origin,
                                   float heightUU,
                                   float& bx, float& by, float& bw, float& bh)
    {
        SDK::FVector top    = { origin.X, origin.Y, origin.Z + heightUU * 0.5f };
        SDK::FVector bottom = { origin.X, origin.Y, origin.Z - heightUU * 0.5f };

        SDK::FVector2D sTop, sBot;
        if (!Helpers::W2S(top, sTop))    return false;
        if (!Helpers::W2S(bottom, sBot)) return false;

        float h = fabsf(static_cast<float>(sTop.Y - sBot.Y));
        float w = h * 0.55f;
        bx = static_cast<float>(sTop.X) - w * 0.5f;
        by = (sTop.Y < sBot.Y) ? static_cast<float>(sTop.Y) : static_cast<float>(sBot.Y);
        bw = w; bh = h;
        return (h > 4.f);
    }

    static void DrawCornerBox(ImDrawList* dl,
                               float x, float y, float w, float h,
                               ImU32 col, float thick, float cLen)
    {
        dl->AddLine({x,         y        }, {x+cLen,    y        }, col, thick);
        dl->AddLine({x,         y        }, {x,         y+cLen   }, col, thick);
        dl->AddLine({x+w,       y        }, {x+w-cLen,  y        }, col, thick);
        dl->AddLine({x+w,       y        }, {x+w,       y+cLen   }, col, thick);
        dl->AddLine({x,         y+h      }, {x+cLen,    y+h      }, col, thick);
        dl->AddLine({x,         y+h      }, {x,         y+h-cLen }, col, thick);
        dl->AddLine({x+w,       y+h      }, {x+w-cLen,  y+h      }, col, thick);
        dl->AddLine({x+w,       y+h      }, {x+w,       y+h-cLen }, col, thick);
    }

    static void DrawBar(ImDrawList* dl,
                         float x, float y, float w, float h,
                         float pct, ImU32 fillColor)
    {
        dl->AddRectFilled({x,     y    }, {x+w,         y+h}, IM_COL32(0,0,0,150));
        dl->AddRectFilled({x,     y    }, {x+w*pct,     y+h}, fillColor);
        dl->AddRect      ({x,     y    }, {x+w,         y+h}, IM_COL32(0,0,0,200), 0.f, 0, 0.8f);
    }

    static ImU32 HealthGradientColor(float hp01)
    {
        hp01 = std::clamp(hp01, 0.f, 1.f);
        int r = 0, g = 0, b = 0;
        if (hp01 <= 0.5f)
        {
            const float t = hp01 / 0.5f;
            r = 220;
            g = (int)(45.f + t * 165.f);
            b = 45;
        }
        else
        {
            const float t = (hp01 - 0.5f) / 0.5f;
            r = (int)(220.f - t * 170.f);
            g = (int)(210.f + t * 10.f);
            b = (int)(45.f + t * 25.f);
        }
        return IM_COL32(r, g, b, 255);
    }

    static void DrawGradientHealthBar(ImDrawList* dl,
                                      float x, float y, float w, float h,
                                      float pct)
    {
        pct = std::clamp(pct, 0.f, 1.f);

        dl->AddRectFilled({ x, y }, { x + w, y + h }, IM_COL32(0, 0, 0, 150));

        if (pct > 0.001f)
        {
            const float fillTop = y + h * (1.f - pct);
            const float fillBot = y + h;
            const ImU32 topCol = HealthGradientColor(pct);
            const ImU32 botCol = HealthGradientColor(0.f);
            dl->AddRectFilledMultiColor(
                { x, fillTop }, { x + w, fillBot },
                topCol, topCol, botCol, botCol);
        }

        dl->AddRect({ x, y }, { x + w, y + h }, IM_COL32(0, 0, 0, 200), 0.f, 0, 0.8f);
    }

    static const Color4* ColorForCategory(PawnFilter::EspCategory cat)
    {
        switch (cat)
        {
        case PawnFilter::EspCategory::EnemyGod:  return &cfg.EnemyColor;
        case PawnFilter::EspCategory::AllyGod:   return &cfg.AllyColor;
        case PawnFilter::EspCategory::Minion:    return &cfg.MinionColor;
        case PawnFilter::EspCategory::Jungle:    return &cfg.JungleColor;
        case PawnFilter::EspCategory::Structure: return &cfg.StructureColor;
        default: return nullptr;
        }
    }

    static bool PassesVisibilityFilter(SDK::AHWCharacter_Base* ch)
    {
        if (!cfg.VisibleCheck) return true;
        return Helpers::IsVisibleToLocal(ch);
    }

    static void DrawAbilityCooldowns(ImDrawList* dl,
                                      SDK::AHWCharacter_Base* ch,
                                      float bx, float by, float bw, float bh, float worldTime)
    {
        if (!cfg.ShowAbilityCooldowns || !ch)
            return;

        const double now = ImGui::GetTime();
        auto& data = DataFor(ch, now);
        const bool refreshCodes = data.codesAt < 0.0 || now - data.codesAt >= 1.0;
        if (refreshCodes)
        {
            data.godCode = AbilityIcons::ResolveGodCode(static_cast<SDK::APawn*>(ch));
            data.codesAt = now;
        }
        if (data.cooldownAt < 0.0 || now - data.cooldownAt >= 0.1 || worldTime < data.sampleWorldTime)
        {
            for (auto& slot : data.slots) slot = {};
            for (auto& slot : data.relic) slot = {};
            EspCooldownSeh::GetEnemyAbilityStates(static_cast<SDK::APawn*>(ch), worldTime, data.slots);
            char hint[48]{};
            EspCooldownSeh::GetEnemyRelicStates(static_cast<SDK::APawn*>(ch), worldTime, data.relic, hint);
            if (hint[0]) data.relicCode = hint;
            else if (refreshCodes) data.relicCode = AbilityIcons::ResolveRelicCode(static_cast<SDK::APawn*>(ch));
            data.relicCode = AbilityIcons::CanonicalizeRelicCode(data.relicCode);
            data.sampleWorldTime = worldTime;
            data.cooldownAt = now;
        }
        const std::string& godCode = data.godCode;
        const std::string& relicCode = data.relicCode;
        EspCooldownSeh::EspAbilitySlotInfo slots[EspCooldownSeh::kMaxCooldowns];
        EspCooldownSeh::EspAbilitySlotInfo relicSlots[EspCooldownSeh::kMaxRelicSlots];
        const float elapsed = fmaxf(0.f, worldTime - data.sampleWorldTime);
        for (int i = 0; i < EspCooldownSeh::kMaxCooldowns; ++i)
        {
            slots[i] = data.slots[i]; AdvanceCooldown(slots[i], elapsed);
        }
        for (int i = 0; i < EspCooldownSeh::kMaxRelicSlots; ++i)
        {
            relicSlots[i] = data.relic[i]; AdvanceCooldown(relicSlots[i], elapsed);
        }
        if (!relicCode.empty() && relicSlots[0].state == EspCooldownSeh::EspAbilitySlotState::Locked)
        {
            relicSlots[0].slot = 1;
            relicSlots[0].state = EspCooldownSeh::EspAbilitySlotState::Ready;
        }

        float distScale = 1.f;
        if (bh > 4.f)
        {
            distScale = bh / 72.f;
            if (distScale < 0.9f) distScale = 0.9f;
            if (distScale > 1.35f) distScale = 1.35f;
        }
        const float scale = cfg.AbilityCooldownScale * distScale;

        const float sideBarW = 4.f;
        const float sideBarGap = 2.f;
        const float pad = 5.f * scale;
        const float gap = 4.f * scale;
        const float iconSize = 34.f * scale;

        float listX = bx + bw + sideBarGap;
        if (cfg.ShowManaBar)
            listX += sideBarW + sideBarGap;
        listX += 6.f;

        const float listY = by;
        AbilityIcons::IconView icons[EspCooldownSeh::kMaxCooldowns]{};

        // Keep texture ownership in AbilityIcons. Only cache availability, not
        // GPU handles. Back off failed disk/remote lookups instead of retrying
        // missing icons every render frame.
        const bool retryIcons = data.iconRetryAt < 0.0 || now - data.iconRetryAt >= 0.25;
        for (int i = 0; i < EspCooldownSeh::kMaxCooldowns; ++i)
        {
            const int slotNum = slots[i].slot > 0 ? slots[i].slot : (i + 1);
            if (data.hasAbilityIcon[i] || retryIcons)
            {
                icons[i] = AbilityIcons::GetAbilityIcon(godCode, slotNum);
                data.hasAbilityIcon[i] = icons[i].valid;
            }
        }

        AbilityIcons::IconView relicIcon{};
        if (!relicCode.empty() && (data.hasRelicIcon || retryIcons))
        {
            relicIcon = AbilityIcons::GetRelicIcon(relicCode);
            data.hasRelicIcon = relicIcon.valid;
        }
        if (retryIcons) data.iconRetryAt = now;

        AbilityCdUi::DrawEspPanel(
            dl, { listX, listY }, iconSize, gap, pad, icons, slots,
            (relicIcon.valid || !relicCode.empty()) ? &relicIcon : nullptr,
            (relicSlots[0].state != EspCooldownSeh::EspAbilitySlotState::Locked ||
             !relicCode.empty())
                ? &relicSlots[0] : nullptr);
    }

    static bool FastCharacterBox(SDK::AHWCharacter_Base* ch, const SDK::FVector& location,
                                 float& bx, float& by, float& bw, float& bh)
    {
        __try {
            SDK::FVector origin{}, extent{};
            ch->GetActorBounds(true, &origin, &extent, false);
            const float height = static_cast<float>(extent.Z) * 2.f;
            if (std::isfinite(height) && height > 4.f && height < 10000.f)
                return CapsuleBBoxScreen(origin, height, bx, by, bw, bh);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return CapsuleBBoxScreen(location, 184.f, bx, by, bw, bh);
    }

    static void DrawChar(ImDrawList* dl,
                          SDK::AHWCharacter_Base* ch,
                          SDK::AHWCharacter_Base* localChar,
                          const Color4& boxCol,
                          bool isGod,
                          bool isEnemyGod, const SDK::FVector& location,
                          float distanceUU, float worldTime)
    {
        float bx, by, bw, bh;
        if (!FastCharacterBox(ch, location, bx, by, bw, bh)) return;
        // Skip metadata/ability calls for boxes completely outside the viewport.
        if (bx + bw < -64.f || bx > ScreenW() + 64.f || by + bh < -64.f || by > ScreenH() + 64.f) return;

        ImU32 col32  = ToU32(boxCol);
        float thick  = cfg.BoxThickness;
        float cLen   = cfg.CornerLength;

        if (cfg.ShowSkeleton && isGod)
            Bones::DrawFilteredSkeleton(dl, ch, ToU32(cfg.SkeletonColor));

        if (cfg.ShowBox)
        {
            switch (cfg.BoxMode)
            {
            case BoxStyle::Full:
                dl->AddRect({ bx, by }, { bx+bw, by+bh }, col32, 0.f, 0, thick);
                break;
            case BoxStyle::Corners:
                DrawCornerBox(dl, bx, by, bw, bh, col32, thick, cLen);
                break;
            case BoxStyle::Lines2D:
                dl->AddLine({ bx,       by+bh*0.5f }, { bx+bw,      by+bh*0.5f }, col32, thick);
                dl->AddLine({ bx+bw*0.5f,by        }, { bx+bw*0.5f, by+bh       }, col32, thick);
                break;
            }
        }

        const float barW = 4.f, barGap = 2.f;

        if (cfg.ShowHealthBar)
        {
            float hp = Helpers::GetHealth(ch), mhp = Helpers::GetMaxHealth(ch);
            float pct = (mhp > 0.f) ? hp / mhp : 0.f;
            DrawGradientHealthBar(dl, bx - barW - barGap, by, barW, bh, pct);
        }
        if (cfg.ShowManaBar)
        {
            float mp = Helpers::GetMana(ch), mmp = Helpers::GetMaxMana(ch);
            float pct = (mmp > 0.f) ? mp / mmp : 0.f;
            DrawBar(dl, bx + bw + barGap, by, barW, bh, pct, ToU32(cfg.ManaColor));
        }

        if (cfg.ShowName)
        {
            const double now = ImGui::GetTime();
            auto& data = DataFor(ch, now);
            if (data.nameAt < 0.0 || now - data.nameAt >= 1.0)
            {
                data.playerName = Helpers::GetPlayerName(ch);
                data.nameAt = now;
            }
            if (!data.playerName.empty())
                dl->AddText({ bx, by - 15.f }, ToU32(cfg.TextColor), data.playerName.c_str());
        }

        float yBelow = by + bh + 2.f;

        if (cfg.ShowLevel)
        {
            float lvl = Helpers::GetLevel(ch);
            if (lvl > 0.f)
            {
                char buf[24]; snprintf(buf, sizeof(buf), "Lv%.0f", lvl);
                dl->AddText({ bx, yBelow }, ToU32(cfg.TextColor), buf);
                yBelow += 14.f;
            }
        }

        if (cfg.ShowDistance && localChar)
        {
            float d = distanceUU;
            char buf[24]; snprintf(buf, sizeof(buf), "%.0fm", d / 100.f);
            dl->AddText({ bx, yBelow }, ToU32(cfg.TextColor), buf);
            yBelow += 14.f;
        }

        if (isEnemyGod)
            DrawAbilityCooldowns(dl, ch, bx, by, bw, bh, worldTime);

        if (cfg.ShowCCIndicator)
        {
            const bool stunned = ch->IsStunned();
            if (stunned || ch->IsHardRooted())
            {
                const char* lbl = stunned ? "[STUN]" : "[ROOT]";
                dl->AddText({ bx + bw * 0.5f - 20.f, by + bh * 0.5f - 7.f },
                            IM_COL32(255, 80, 80, 255), lbl);
            }
        }

        if (cfg.ShowSnaplines)
        {
            dl->AddLine({ ScreenW() * 0.5f, ScreenH() },
                        { bx + bw * 0.5f, by + bh     },
                        ToU32(cfg.SnaplinesColor), 1.f);
        }
    }

    static void DrawActorCapsule(ImDrawList* dl,
                                  SDK::AActor* actor,
                                  SDK::AHWCharacter_Base* localChar,
                                  const Color4& col,
                                  float heightUU = 184.f)
    {
        float bx, by, bw, bh;
        if (!CapsuleBBoxScreen(Helpers::GetLocation(actor), heightUU, bx, by, bw, bh))
            return;

        ImU32 col32 = ToU32(col);
        if (cfg.ShowBox)
            DrawCornerBox(dl, bx, by, bw, bh, col32, cfg.BoxThickness, cfg.CornerLength);

        if (cfg.ShowDistance && localChar)
        {
            float d = Helpers::Dist3D(Helpers::GetLocation(actor), Helpers::GetLocation(localChar));
            char buf[24]; snprintf(buf, sizeof(buf), "%.0fm", d / 100.f);
            dl->AddText({ bx, by + bh + 2.f }, ToU32(cfg.TextColor), buf);
        }
    }

    static void DrawWardRadius(ImDrawList* dl,
                              SDK::AHWDeployable_Ward* ward,
                              SDK::APlayerController* pc,
                              const SDK::FVector& loc)
    {
        float radiusCm = 1371.6f;
        __try {
            SDK::USphereComponent* sphere = ward ? ward->VisionRangeCollision : nullptr;
            if (sphere)
            {
                float r = sphere->GetScaledSphereRadius();
                if (r > 1.0f)
                    radiusCm = r;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        constexpr int segments = 48;
        ImVec2 pts[segments];
        int numPts = 0;
        for (int j = 0; j < segments; ++j)
        {
            float angle = static_cast<float>(j) / static_cast<float>(segments) * 6.2831853f;
            SDK::FVector ptWorld{
                loc.X + radiusCm * cosf(angle),
                loc.Y + radiusCm * sinf(angle),
                loc.Z
            };
            SDK::FVector2D ptScr{};
            if (pc->ProjectWorldLocationToScreen(ptWorld, &ptScr, true))
                pts[numPts++] = ImVec2(static_cast<float>(ptScr.X), static_cast<float>(ptScr.Y));
        }

        if (numPts < 3) return;
        dl->AddConvexPolyFilled(pts, numPts, IM_COL32(0, 0, 0, 60));
        dl->AddPolyline(pts, numPts, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 1.0f);
    }

    static void DrawWard(ImDrawList* dl,
                          SDK::AHWDeployable_Ward* ward,
                          SDK::APawn* localPawn,
                          SDK::APlayerController* pc,
                          SDK::AHWCharacter_Base* localChar)
    {
        __try {
            if (ward->bIsDead) return;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        SDK::ETeamAttitude att = Helpers::GetTeamAttitude(localPawn, static_cast<SDK::AActor*>(ward));
        const bool isEnemy    = (att == SDK::ETeamAttitude::Hostile);
        const bool isFriendly = (att == SDK::ETeamAttitude::Friendly);

        if ((isEnemy && !cfg.ShowEnemyWards) || (isFriendly && !cfg.ShowFriendlyWards))
            return;

        if (cfg.VisibleCheck && !Helpers::IsWardVisibleToLocal(ward)) return;

        SDK::FVector loc = Helpers::GetLocation(ward);
        SDK::FVector2D s{};
        if (!pc->ProjectWorldLocationToScreen(loc, &s, true)) return;

        ImVec2 p(static_cast<float>(s.X), static_cast<float>(s.Y));
        ImU32 wardCol = isEnemy
            ? ToU32(cfg.WardColor)
            : (isFriendly ? ToU32(cfg.FriendlyWardColor) : IM_COL32(200, 200, 200, 255));
        dl->AddCircle(p, 9.f, wardCol, 24, 1.5f);

        if (cfg.ShowWardRadius)
            DrawWardRadius(dl, ward, pc, loc);

        if (cfg.ShowName)
        {
            const char* label = isFriendly ? "FRIENDLY" : (isEnemy ? "ENEMY WARD" : "NEUTRAL WARD");
            ImU32 labelCol = isFriendly
                ? ToU32(cfg.FriendlyWardColor)
                : (isEnemy ? ToU32(cfg.WardColor) : IM_COL32(200, 200, 200, 255));
            dl->AddText({ p.x + 12.f, p.y - 6.f }, labelCol, label);
        }

        if (cfg.ShowDistance && localChar)
        {
            float d = Helpers::Dist3D(loc, Helpers::GetLocation(localChar));
            char buf[24]; snprintf(buf, sizeof(buf), "%.0fm", d / 100.f);
            dl->AddText({ p.x, p.y + 12.f }, ToU32(cfg.TextColor), buf);
        }
    }

    static void ProcessEspActorSafe(ImDrawList* dl,
                                    SDK::AActor* actor,
                                    SDK::APawn* localPawn,
                                    SDK::APlayerController* pc,
                                    SDK::AHWCharacter_Base* localChar,
                                    const SDK::FVector& localPos, float worldTime)
    {
        __try {
            if (!actor || !Helpers::IsValidPtr(actor))
                return;
            if (localPawn && actor == localPawn)
                return;

            if (cfg.ShowWards && actor->IsA(SDK::AHWDeployable_Ward::StaticClass()))
            {
                float dist = Helpers::Dist3D(Helpers::GetLocation(actor), localPos);
                if (dist <= cfg.MaxDistance)
                {
                    DrawWard(dl, static_cast<SDK::AHWDeployable_Ward*>(actor),
                             localPawn, pc, localChar);
                }
                return;
            }

            if (!actor->IsA(SDK::APawn::StaticClass()))
                return;

            const SDK::FVector location = Helpers::GetLocation(actor);
            const float dist = Helpers::Dist3D(location, localPos);
            const float widestRange = fmaxf(cfg.MaxDistance, cfg.ShowJungle ? cfg.JungleMaxDistance : 0.f);
            if (dist > widestRange) return;

            const double now = ImGui::GetTime();
            auto& data = DataFor(actor, now);
            if (data.categoryAt < 0.0 || now - data.categoryAt >= 0.2)
            {
                data.category = PawnFilter::EspCategoryFor(actor, localPawn,
                    cfg.ShowEnemyGods, cfg.ShowAllyGods, cfg.ShowMinions, cfg.ShowJungle, cfg.ShowStructures);
                data.categoryAt = now;
            }
            const PawnFilter::EspCategory cat = data.category;
            if (cat == PawnFilter::EspCategory::Skip) return;
            const float maxDist = cat == PawnFilter::EspCategory::Jungle ? cfg.JungleMaxDistance : cfg.MaxDistance;
            if (dist > maxDist) return;

            const Color4* color = ColorForCategory(cat);
            if (!color)
                return;

            const bool isGod = (cat == PawnFilter::EspCategory::EnemyGod ||
                                cat == PawnFilter::EspCategory::AllyGod);

            if (isGod && actor->IsA(SDK::AHWCharacter_Base::StaticClass()))
            {
                auto* ch = static_cast<SDK::AHWCharacter_Base*>(actor);
                if (Helpers::IsDead(ch) || !PassesVisibilityFilter(ch))
                    return;
                DrawChar(dl, ch, localChar, *color, true,
                         cat == PawnFilter::EspCategory::EnemyGod, location, dist, worldTime);
            }
            else
            {
                DrawActorCapsule(dl, actor, localChar, *color,
                                 (cat == PawnFilter::EspCategory::Structure) ? 260.f : 184.f);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void Tick()
    {
        if (!cfg.Enabled || !GameState::IsPlayable())
        {
            Reset();
            return;
        }
        uintptr_t world = 0, level = 0;
        if (!ReadWorldStamp(world, level)) { Reset(); return; }

        auto* pc = Helpers::GetLocalPC();
        auto* localChar = Helpers::GetLocalChar();
        if (localChar && Helpers::IsDead(localChar))
            return;

        SDK::APawn* localPawn = Helpers::SafeGetPawnFromPC(pc);
        const unsigned filterMask = (cfg.ShowEnemyGods ? 1u : 0u) | (cfg.ShowAllyGods ? 2u : 0u) |
            (cfg.ShowMinions ? 4u : 0u) | (cfg.ShowJungle ? 8u : 0u) | (cfg.ShowStructures ? 16u : 0u);
        const uintptr_t pawnKey = reinterpret_cast<uintptr_t>(localPawn);
        if (world != s_world || level != s_level || pawnKey != s_localPawn || filterMask != s_filterMask)
        {
            Reset(); s_world = world; s_level = level; s_localPawn = pawnKey; s_filterMask = filterMask;
        }
        if (filterMask == 0 && !cfg.ShowWards) { s_data.clear(); return; }
        const double now = ImGui::GetTime();
        if (now - s_pruneAt >= 1.0)
        {
            for (auto it = s_data.begin(); it != s_data.end(); )
                if (now - it->second.lastSeen > 2.0) it = s_data.erase(it); else ++it;
            s_pruneAt = now;
        }
        SDK::FVector localPos = localPawn ? Helpers::GetLocation(localPawn) : SDK::FVector{};

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl || !pc)
            return;

        const float worldTime = cfg.ShowAbilityCooldowns ? Helpers::GetWorldTimeSeconds() : 0.f;
        Helpers::GatherLevelActors(s_frameActors);
        for (SDK::AActor* actor : s_frameActors)
            ProcessEspActorSafe(dl, actor, localPawn, pc, localChar, localPos, worldTime);
        s_frameActors.clear();
    }
}
