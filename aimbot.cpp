#include "aimbot.h"
#include "aimbot_prediction.h"
#include "../core/sdk_helpers.h"
#include "../core/game_state.h"
#include "../core/pawn_filter.h"
#include "../core/gamepad_input.h"
#include "../imgui/imgui.h"
#include <cmath>
#include <cfloat>
#include <unordered_set>
#include <string>

namespace Aimbot
{
    Config cfg;

    static bool  s_hasImpact = false;
    static ImVec2 s_impactPos{};
    static bool  s_hasTargetInfo = false;
    static std::string s_targetName;
    static float s_targetDistM = 0.f;
    static SDK::APawn* s_lastTarget = nullptr;
    static AimPrediction::VelocityFilter s_velocityFilter;

    static float s_inhandModPct = 0.f;
    static float s_effectiveProjSpeed = 1700.f;
    static float s_recommendedProjSpeed = 1700.f;
    static bool  s_hasSpeedState = false;
    static std::string s_recommendedSpeedText;

    static float GetInhandSpeedModifierPercent()
    {
        SDK::AHWCharacter_Base* ch = Helpers::GetLocalChar();
        if (!ch)
            return 0.f;

        __try {
            return ch->GetInhandProjectileSpeedModifierPercent();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0.f;
        }
    }

    static void RefreshProjectileSpeedState()
    {
        s_hasSpeedState = false;
        s_inhandModPct = 0.f;
        s_effectiveProjSpeed = cfg.ProjectileSpeed;
        s_recommendedProjSpeed = cfg.ProjectileSpeed;
        s_recommendedSpeedText.clear();

        SDK::AHWCharacter_Base* ch = Helpers::GetLocalChar();
        if (!ch)
            return;

        s_hasSpeedState = true;
        s_inhandModPct = GetInhandSpeedModifierPercent();
        s_recommendedProjSpeed = cfg.ProjectileSpeed * (1.f + s_inhandModPct * 0.01f);

        if (cfg.UseRecommendedSpeed)
            s_effectiveProjSpeed = s_recommendedProjSpeed;
        else
            s_effectiveProjSpeed = cfg.ProjectileSpeed;

        char buf[192];
        snprintf(buf, sizeof(buf),
                 "Projectile speed: %.0f cm/s  (base %.0f, modifier %+.0f%%)%s",
                 s_recommendedProjSpeed,
                 cfg.ProjectileSpeed,
                 s_inhandModPct,
                 cfg.UseRecommendedSpeed ? "  [auto ON]" : "");
        s_recommendedSpeedText = buf;
    }

    static float EffectiveProjectileSpeed()
    {
        return s_effectiveProjSpeed;
    }

    static float ComputeDynamicSmoothing(float baseSmoothing,
                                         float distCm,
                                         float rangeMeters)
    {
        float sm = (baseSmoothing <= 1.f) ? 1.f : baseSmoothing;
        if (cfg.DynamicSmoothScale <= 0.f)
            return sm;

        float rangeCm = (rangeMeters > 0.1f) ? rangeMeters * 100.f : 3000.f;
        float ratio = distCm / rangeCm;
        if (ratio < 0.5f) ratio = 0.5f;
        if (ratio > 1.f) ratio = 1.f;

        float scaled = (ratio - 0.5f) * 2.f;
        float dynamicSm = sm * (1.f - cfg.DynamicSmoothScale * scaled);
        float minSm = sm * 0.1f;
        if (dynamicSm < minSm)
            dynamicSm = minSm;
        return dynamicSm;
    }

    static bool IsHoldBindingDown()
    {
        if (cfg.HoldBind == HoldBindType::Keyboard)
            return (GetAsyncKeyState(cfg.HoldKey) & 0x8000) != 0;

        if (cfg.HoldPadButton < 0 ||
            cfg.HoldPadButton >= GamepadInput::ButtonCount())
            return false;

        return GamepadInput::IsDeviceButtonDown(
            cfg.HoldPadDevice,
            static_cast<GamepadInput::Button>(cfg.HoldPadButton));
    }

    static bool IsToggleBindConfigured()
    {
        if (cfg.ToggleBind == HoldBindType::Keyboard)
            return cfg.ToggleKey != 0;

        if (cfg.TogglePadButton < 0 ||
            cfg.TogglePadButton >= GamepadInput::ButtonCount())
            return false;

        return true;
    }

    static bool IsToggleBindingDown()
    {
        if (!IsToggleBindConfigured())
            return false;

        if (cfg.ToggleBind == HoldBindType::Keyboard)
            return (GetAsyncKeyState(cfg.ToggleKey) & 0x8000) != 0;

        return GamepadInput::IsDeviceButtonDown(
            cfg.TogglePadDevice,
            static_cast<GamepadInput::Button>(cfg.TogglePadButton));
    }

    static bool s_toggleLatched = false;
    static bool s_toggleBindWasDown = false;
    static bool s_holdBindToggledOn = false;
    static bool s_holdBindWasDown = false;

    static void UpdateToggleHotkey()
    {
        if (!IsToggleBindConfigured())
        {
            s_toggleLatched = false;
            s_toggleBindWasDown = false;
            return;
        }

        if (!cfg.Enabled)
            return;

        const bool down = IsToggleBindingDown();
        if (down && !s_toggleBindWasDown)
            s_toggleLatched = !s_toggleLatched;
        s_toggleBindWasDown = down;
    }

    static bool IsNoPredBindConfigured()
    {
        if (cfg.NoPredBind == HoldBindType::Keyboard)
            return cfg.NoPredKey != 0;

        if (cfg.NoPredPadButton < 0 ||
            cfg.NoPredPadButton >= GamepadInput::ButtonCount())
            return false;

        return true;
    }

    static bool IsNoPredBindingDown()
    {
        if (!IsNoPredBindConfigured())
            return false;

        if (cfg.NoPredBind == HoldBindType::Keyboard)
            return (GetAsyncKeyState(cfg.NoPredKey) & 0x8000) != 0;

        return GamepadInput::IsDeviceButtonDown(
            cfg.NoPredPadDevice,
            static_cast<GamepadInput::Button>(cfg.NoPredPadButton));
    }

    static bool s_usePrediction = true;

    static bool IsHoldBindToggleActive()
    {
        const bool bindingDown = IsHoldBindingDown();
        if (bindingDown && !s_holdBindWasDown)
            s_holdBindToggledOn = !s_holdBindToggledOn;
        s_holdBindWasDown = bindingDown;
        return s_holdBindToggledOn;
    }

    static bool IsAimActive()
    {
        s_usePrediction = true;

        if (IsNoPredBindingDown())
        {
            s_usePrediction = false;
            return true;
        }

        bool active = IsHoldBindingDown();

        if (s_toggleLatched)
            active = true;

        return active;
    }

    struct BallisticLeadResult
    {
        SDK::FVector relPos{};
        SDK::FVector relPred{};
        SDK::FVector predWorld{};
        float interceptTime = 0.f;
    };

    // Straight-line intercept using target velocity only. No gravity or latency.
    // If no reasonable intercept exists, aim at the current target location.
    static BallisticLeadResult ComputeBallisticLead(const SDK::FVector& myLoc,
                                                     const SDK::FVector& tgtLoc,
                                                     const SDK::FVector& tgtVel,
                                                     float projSpeed,
                                                     float leadScale)
    {
        BallisticLeadResult out{};
        out.relPos = {
            tgtLoc.X - myLoc.X,
            tgtLoc.Y - myLoc.Y,
            tgtLoc.Z - myLoc.Z
        };

        SDK::FVector scaledVel{
            tgtVel.X * leadScale,
            tgtVel.Y * leadScale,
            tgtVel.Z * leadScale
        };

        double intercept = 0.0;
        const AimPrediction::Vec3 pos{out.relPos.X, out.relPos.Y, out.relPos.Z};
        AimPrediction::Vec3 vel{scaledVel.X, scaledVel.Y, scaledVel.Z};
        if (!AimPrediction::Finite(vel))
        {
            scaledVel = {};
            vel = {};
        }
        AimPrediction::InterceptTime(pos, vel, projSpeed, intercept);
        const float t = static_cast<float>(intercept);

        out.interceptTime = t;
        out.relPred = {
            out.relPos.X + scaledVel.X * t,
            out.relPos.Y + scaledVel.Y * t,
            out.relPos.Z + scaledVel.Z * t
        };
        out.predWorld = {
            tgtLoc.X + scaledVel.X * t,
            tgtLoc.Y + scaledVel.Y * t,
            tgtLoc.Z + scaledVel.Z * t
        };
        return out;
    }

    static bool IsHardLockTargetUsable(SDK::APawn* target)
    {
        if (!target || !Helpers::IsValidPtr(target) || !PawnFilter::IsHeroPawn(target))
            return false;

        __try {
            if (Helpers::SafeIsA(target, SDK::AHWCharacter_Base::StaticClass()))
            {
                auto* ch = static_cast<SDK::AHWCharacter_Base*>(target);
                if (Helpers::IsDead(ch))
                    return false;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static void TryScoreAimbotPawn(SDK::APawn* pawn,
                                   SDK::APawn* mePawn,
                                   SDK::APlayerController* pc,
                                   const SDK::FVector& myLoc,
                                   float maxRangeSq,
                                   float radiusSqPx,
                                   const ImVec2& center,
                                   float projSpeed,
                                   bool teamCheck,
                                   bool usePrediction,
                                   SDK::APawn** outBest,
                                   float* outBestMetric)
    {
        __try {
            if (!pawn || !Helpers::IsValidPtr(pawn) ||
                !mePawn || !Helpers::IsValidPtr(mePawn) ||
                !pc || !Helpers::IsValidPtr(pc))
                return;

            if (!PawnFilter::IsHeroPawn(pawn))
                return;

            if (teamCheck && !Helpers::IsHostileToLocal(mePawn, pawn))
                return;

            SDK::FVector tgtLoc = Helpers::GetLocation(pawn);
            float dx = tgtLoc.X - myLoc.X;
            float dy = tgtLoc.Y - myLoc.Y;
            float dz = tgtLoc.Z - myLoc.Z;
            float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq > maxRangeSq)
                return;

            SDK::FVector aimWorld = tgtLoc;
            if (usePrediction)
            {
                SDK::FVector tgtVel = Helpers::GetPawnVelocity(pawn);
                BallisticLeadResult lead = ComputeBallisticLead(
                    myLoc, tgtLoc, tgtVel, projSpeed, cfg.LeadScale);
                aimWorld = lead.predWorld;

                float px = aimWorld.X - myLoc.X;
                float py = aimWorld.Y - myLoc.Y;
                float pz = aimWorld.Z - myLoc.Z;
                float predDistSq = px * px + py * py + pz * pz;
                if (predDistSq > maxRangeSq)
                    return;
            }

            SDK::FVector2D sp{};
            if (!pc->ProjectWorldLocationToScreen(aimWorld, &sp, true))
                return;

            float sx = static_cast<float>(sp.X) - center.x;
            float sy = static_cast<float>(sp.Y) - center.y;
            float screenDistSq = sx * sx + sy * sy;
            if (screenDistSq > radiusSqPx)
                return;

            float metric = screenDistSq;
            if (cfg.TargetPri == TargetMode::LowestHealth &&
                Helpers::SafeIsA(pawn, SDK::AHWCharacter_Base::StaticClass()))
            {
                auto* ch = static_cast<SDK::AHWCharacter_Base*>(pawn);
                metric = ch->GetHealthPercentage();
            }

            if (metric < *outBestMetric)
            {
                *outBestMetric = metric;
                *outBest = pawn;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static SDK::APawn* AcquireBestTarget(SDK::APlayerController* pc,
                                          float fovDeg,
                                          float rangeMeters,
                                          float projSpeed,
                                          bool teamCheck,
                                          bool usePrediction)
    {
        if (!pc || !GameState::IsPlayable())
            return nullptr;

        SDK::APawn* mePawn = Helpers::SafeGetPawnFromPC(pc);
        if (!mePawn || !Helpers::IsValidPtr(mePawn))
            return nullptr;

        float screenWidth = ImGui::GetIO().DisplaySize.x;
        float radiusPx = (fovDeg / 90.f) * (screenWidth * 0.5f);
        float radiusSqPx = radiusPx * radiusPx;
        ImVec2 center = Helpers::GetDisplayCenter();

        float maxRangeCm = rangeMeters * 100.f;
        float maxRangeSq = maxRangeCm * maxRangeCm;
        SDK::FVector myLoc = Helpers::GetLocation(mePawn);

        std::unordered_set<SDK::APawn*> unique;
        SDK::APawn* bestPawn = nullptr;
        float bestMetric = FLT_MAX;

        for (SDK::APawn* pawn : Helpers::GetAllPawns())
        {
            if (!unique.insert(pawn).second)
                continue;

            TryScoreAimbotPawn(pawn, mePawn, pc, myLoc, maxRangeSq, radiusSqPx, center,
                                projSpeed, teamCheck, usePrediction,
                                &bestPawn, &bestMetric);
        }

        return bestPawn;
    }

    struct AimStepResult
    {
        bool  ok = false;
        float yawStep = 0.f;
        float impactX = 0.f;
        float impactY = 0.f;
        bool  hasImpact = false;
        float targetDistM = 0.f;
    };

    static AimStepResult ComputeAimStepSeh(SDK::APlayerController* pc,
                                           SDK::APawn* target,
                                           float smoothing,
                                           float projSpeed,
                                           float rangeMeters,
                                           bool usePrediction)
    {
        AimStepResult result{};
        __try {
            if (!pc || !target || !Helpers::IsValidPtr(pc) || !Helpers::IsValidPtr(target))
                return result;

            SDK::APawn* mePawn = Helpers::SafeGetPawnFromPC(pc);
            if (!mePawn || !Helpers::IsValidPtr(mePawn))
                return result;

            SDK::FVector meLoc = Helpers::GetLocation(mePawn);
            SDK::FVector tgtLoc = Helpers::GetLocation(target);

            SDK::FVector relPos{
                tgtLoc.X - meLoc.X,
                tgtLoc.Y - meLoc.Y,
                tgtLoc.Z - meLoc.Z
            };

            SDK::FVector aimWorld = tgtLoc;
            SDK::FVector relAim = relPos;
            if (usePrediction)
            {
                SDK::FVector rawVel = Helpers::GetPawnVelocity(target);
                const AimPrediction::Vec3 sample{rawVel.X, rawVel.Y, rawVel.Z};
                const AimPrediction::Vec3 filtered = s_velocityFilter.Update(target, sample, GetTickCount64());
                SDK::FVector tgtVel{filtered.x, filtered.y, filtered.z};
                BallisticLeadResult lead = ComputeBallisticLead(
                    meLoc, tgtLoc, tgtVel, projSpeed, cfg.LeadScale);
                aimWorld = lead.predWorld;
                relAim = lead.relPred;
            }

            SDK::FVector2D scr{};
            if (pc->ProjectWorldLocationToScreen(aimWorld, &scr, true))
            {
                result.impactX = static_cast<float>(scr.X);
                result.impactY = static_cast<float>(scr.Y);
                result.hasImpact = true;
            }

            const float dx = relAim.X;
            const float dy = relAim.Y;
            const float dz = relAim.Z;
            const float desiredYaw = atan2f(dy, dx) * (180.f / 3.14159265f);

            SDK::FRotator cur = pc->GetControlRotation();
            const float yawDelta = Helpers::GetYawDelta(cur.Yaw, desiredYaw);

            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            const float dynamicSm = ComputeDynamicSmoothing(smoothing, dist, rangeMeters);

            float yawStep = yawDelta / dynamicSm;
            const float maxStep = 10.f;
            if (yawStep > maxStep) yawStep = maxStep;
            if (yawStep < -maxStep) yawStep = -maxStep;

            result.yawStep = yawStep;
            result.targetDistM = sqrtf(relPos.X * relPos.X +
                                       relPos.Y * relPos.Y +
                                       relPos.Z * relPos.Z) / 100.f;
            result.ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        return result;
    }

    static void ApplyYawInputSeh(SDK::APlayerController* pc, float yawStep)
    {
        __try {
            if (pc && Helpers::IsValidPtr(pc))
                pc->AddYawInput(yawStep);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void AimAtTarget(SDK::APlayerController* pc,
                             SDK::APawn* target,
                             float smoothing,
                             float projSpeed,
                             float rangeMeters,
                             bool usePrediction)
    {
        if (!pc || !target || !GameState::IsPlayable())
            return;

        const AimStepResult step =
            ComputeAimStepSeh(pc, target, smoothing, projSpeed, rangeMeters, usePrediction);
        if (!step.ok)
            return;

        ApplyYawInputSeh(pc, step.yawStep);

        if (step.hasImpact)
        {
            s_impactPos = { step.impactX, step.impactY };
            s_hasImpact = true;
        }

        if (cfg.ShowTargetInfo)
        {
            s_targetName = Helpers::GetPawnDisplayName(target);
            if (s_targetName.empty())
                s_targetName = Helpers::SafeGetActorName(target);
            s_targetDistM = step.targetDistM;
            s_hasTargetInfo = true;
        }
    }

    static void DrawOverlay()
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        if (cfg.ShowFovCircle && cfg.FovDeg > 0.f)
        {
            ImVec2 center = Helpers::GetDisplayCenter();
            float screenWidth = ImGui::GetIO().DisplaySize.x;
            float radiusPx = (cfg.FovDeg / 90.f) * (screenWidth * 0.5f);
            dl->AddCircle(center, radiusPx, IM_COL32(255, 255, 255, 60), 64, 1.f);
        }

        if (cfg.ShowImpactOverlay && s_hasImpact)
        {
            dl->AddCircleFilled(s_impactPos, 8.f, IM_COL32(255, 80, 40, 220));
            dl->AddCircle(s_impactPos, 14.f, IM_COL32(255, 255, 255, 160), 24, 1.5f);
        }

        if (cfg.ShowTargetInfo && s_hasTargetInfo)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s  %.1fm", s_targetName.c_str(), s_targetDistM);
            ImVec2 center = Helpers::GetDisplayCenter();
            dl->AddText({ center.x + 20.f, center.y - 40.f }, IM_COL32(255, 255, 255, 220), buf);
        }

        if (cfg.ShowRecommendedSpeed && s_hasSpeedState && !s_recommendedSpeedText.empty())
        {
            ImVec2 textSz = ImGui::CalcTextSize(s_recommendedSpeedText.c_str());
            float x = (ImGui::GetIO().DisplaySize.x - textSz.x) * 0.5f;
            float y = ImGui::GetIO().DisplaySize.y - textSz.y - 28.f;
            dl->AddText({ x + 1.f, y + 1.f }, IM_COL32(0, 0, 0, 220), s_recommendedSpeedText.c_str());
            dl->AddText({ x, y }, IM_COL32(255, 255, 255, 235), s_recommendedSpeedText.c_str());
        }
    }

    void ResetState()
    {
        s_hasImpact = false;
        s_hasTargetInfo = false;
        s_targetName.clear();
        s_targetDistM = 0.f;
        s_lastTarget = nullptr;
        s_velocityFilter.Reset();
        s_toggleLatched = false;
        s_toggleBindWasDown = false;
        s_holdBindToggledOn = false;
        s_holdBindWasDown = false;
    }

    bool IsToggleLatched()
    {
        return s_toggleLatched;
    }

    void Tick()
    {
        // Per-frame overlay data must be cleared without discarding the lock,
        // toggle state or motion history on every tick.
        s_hasImpact = false;
        s_hasTargetInfo = false;
        s_targetName.clear();
        s_targetDistM = 0.f;

        const bool drawOverlay = cfg.ShowFovCircle || cfg.ShowImpactOverlay ||
                                 cfg.ShowTargetInfo || cfg.ShowRecommendedSpeed;

        if (GameState::IsPlayable())
            UpdateToggleHotkey();

        if (!GameState::IsPlayable())
        {
            ResetState();
            s_hasSpeedState = false;
            if (drawOverlay)
                DrawOverlay();
            return;
        }

        if (cfg.Enabled || cfg.ShowRecommendedSpeed)
            RefreshProjectileSpeedState();

        if (!cfg.Enabled)
        {
            ResetState();
            if (drawOverlay)
                DrawOverlay();
            return;
        }

        auto* pc = Helpers::GetLocalPC();
        if (!pc)
        {
            s_lastTarget = nullptr;
            s_velocityFilter.Reset();
            if (drawOverlay)
                DrawOverlay();
            return;
        }

        auto* localChar = Helpers::GetLocalChar();
        if (!localChar || Helpers::IsDead(localChar))
        {
            s_lastTarget = nullptr;
            s_velocityFilter.Reset();
            if (drawOverlay)
                DrawOverlay();
            return;
        }

        if (!IsAimActive())
        {
            s_lastTarget = nullptr;
            s_velocityFilter.Reset();
            if (drawOverlay)
                DrawOverlay();
            return;
        }

        float projSpeed = EffectiveProjectileSpeed();
        const bool usePrediction = s_usePrediction;
        SDK::APawn* target = nullptr;

        if (cfg.HardLock)
        {
            if (s_lastTarget && IsHardLockTargetUsable(s_lastTarget))
                target = s_lastTarget;

            if (!target)
            {
                s_lastTarget = AcquireBestTarget(pc, cfg.FovDeg, cfg.RangeMeters,
                                                 projSpeed, cfg.TeamCheck, usePrediction);
                target = s_lastTarget;
            }
        }
        else
        {
            s_lastTarget = nullptr;
            target = AcquireBestTarget(pc, cfg.FovDeg, cfg.RangeMeters,
                                       projSpeed, cfg.TeamCheck, usePrediction);
        }

        if (target)
        {
            if (!usePrediction)
                s_velocityFilter.Reset();
            AimAtTarget(pc, target, cfg.Smoothing, projSpeed, cfg.RangeMeters, usePrediction);
        }
        else
            s_velocityFilter.Reset();

        if (drawOverlay)
            DrawOverlay();
    }
}
