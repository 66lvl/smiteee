#include "camera.h"
#include "../core/sdk_helpers.h"
#include "../core/game_state.h"

namespace Camera
{
    Config cfg;

    static SDK::UHWCameraSettings* s_lastSettings = nullptr;
    static SDK::AHWCharacter_PlayableCharacter* s_lastPlayable = nullptr;
    static float s_origArmMax = -1.f;
    static float s_origArmMin = -1.f;
    static bool  s_origArmCollision = true;
    static bool  s_wasFovActive = false;

    static SDK::AHWCharacter_PlayableCharacter* GetLocalPlayable()
    {
        __try {
            auto* pc = Helpers::GetLocalPC();
            if (!pc) return nullptr;
            auto* pawn = pc->K2_GetPawn();
            if (!pawn || !pawn->IsA(SDK::AHWCharacter_PlayableCharacter::StaticClass()))
                return nullptr;
            return static_cast<SDK::AHWCharacter_PlayableCharacter*>(pawn);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    static void RestoreSpringArmCollision(SDK::AHWCharacter_PlayableCharacter* playable, bool collision)
    {
        if (!playable || !Helpers::IsValidPtr(playable))
            return;

        __try {
            SDK::UHWSpringArmComponent* arm = playable->CameraSpringArm;
            if (!arm || !Helpers::IsValidPtr(arm))
                return;
            arm->bDoCollisionTest = collision;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void RestoreCameraSettings(SDK::AHWCharacter_PlayableCharacter* playable)
    {
        if (!s_lastSettings || !Helpers::IsValidPtr(s_lastSettings))
        {
            s_lastSettings = nullptr;
            s_lastPlayable = nullptr;
            s_origArmMax = -1.f;
            s_origArmMin = -1.f;
            return;
        }

        __try {
            if (s_origArmMax >= 0.f)
                s_lastSettings->SpringArmLength_MaxZoom = s_origArmMax;
            if (s_origArmMin >= 0.f)
                s_lastSettings->SpringArmLength_MinZoom = s_origArmMin;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        RestoreSpringArmCollision(s_lastPlayable, s_origArmCollision);

        s_lastSettings = nullptr;
        s_lastPlayable = nullptr;
        s_origArmMax = -1.f;
        s_origArmMin = -1.f;
    }

    static void CacheCameraSettings(SDK::UHWCameraSettings* settings,
                                    SDK::AHWCharacter_PlayableCharacter* playable)
    {
        if (!settings || !Helpers::IsValidPtr(settings))
            return;

        if (s_lastSettings == settings)
            return;

        if (s_lastSettings)
            RestoreCameraSettings(playable);

        __try {
            s_origArmMax = settings->SpringArmLength_MaxZoom;
            s_origArmMin = settings->SpringArmLength_MinZoom;
            s_lastSettings = settings;
            s_lastPlayable = playable;

            if (playable && Helpers::IsValidPtr(playable))
            {
                SDK::UHWSpringArmComponent* arm = playable->CameraSpringArm;
                if (arm && Helpers::IsValidPtr(arm))
                    s_origArmCollision = arm->bDoCollisionTest;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Only the local character's normal camera is safe to modify. Also skip
    // blends to another target: the current target may still be the character.
    static bool CanModifyPlayerCamera(SDK::APlayerController* pc,
                                     SDK::AHWCharacter_PlayableCharacter* playable)
    {
        if (!pc || !playable || !Helpers::IsValidPtr(pc) || !Helpers::IsValidPtr(playable))
            return false;
        __try {
            auto* arm = playable->CameraSpringArm;
            auto* manager = pc->PlayerCameraManager;
            if (!arm || !Helpers::IsValidPtr(arm) || !manager || !Helpers::IsValidPtr(manager))
                return false;
            if (pc->GetViewTarget() != playable || manager->ViewTarget.Target != playable)
                return false;
            auto* pending = manager->PendingViewTarget.Target;
            return !pending || pending == playable;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    static bool ApplyFov(SDK::APlayerController* pc,
                         SDK::AHWCharacter_PlayableCharacter* playable, bool active)
    {
        // FOV(0) also reaches the game's camera code, so restoration needs the
        // same guard. Defer it until the normal player camera is back.
        if (!CanModifyPlayerCamera(pc, playable))
            return false;
        __try {
            if (active)
                pc->FOV(cfg.FovValue);
            else if (s_wasFovActive)
                pc->FOV(0.f);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    static void ApplyDistance(SDK::AHWCharacter_PlayableCharacter* playable,
                              SDK::UHWCameraSettings* settings,
                              bool active)
    {
        if (!playable || !settings || !Helpers::IsValidPtr(settings))
            return;

        if (!active)
        {
            if (s_lastSettings == settings)
                RestoreCameraSettings(playable);
            return;
        }

        CacheCameraSettings(settings, playable);

        __try {
            settings->SpringArmLength_MaxZoom = cfg.DistanceValue;
            settings->SpringArmLength_MinZoom = cfg.DistanceValue;

            if (cfg.NoArmCollision)
            {
                SDK::UHWSpringArmComponent* arm = playable->CameraSpringArm;
                if (arm && Helpers::IsValidPtr(arm))
                    arm->bDoCollisionTest = false;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void Reset()
    {
        if (!s_wasFovActive && !s_lastSettings && !s_lastPlayable) return;
        __try {
            auto* pc = Helpers::GetLocalPC();
            auto* playable = GetLocalPlayable();
            if (ApplyFov(pc, playable, false))
                s_wasFovActive = false;
            RestoreCameraSettings(playable);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Keep a pending FOV restore across cinematic/spectator cameras.
    }

    void Tick()
    {
        if (!GameState::IsPlayable())
        {
            Reset();
            return;
        }

        if (!cfg.EnableFov && !cfg.EnableDistance)
        {
            Reset();
            return;
        }

        auto* pc = Helpers::GetLocalPC();
        auto* playable = GetLocalPlayable();
        if (!CanModifyPlayerCamera(pc, playable))
        {
            RestoreCameraSettings(playable);
            return;
        }
        SDK::UHWCameraSettings* settings = nullptr;

        __try {
            if (playable && Helpers::IsValidPtr(playable))
                settings = playable->CurrentCameraSettings;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        const bool fovActive = cfg.EnableFov;
        if (ApplyFov(pc, playable, fovActive))
            s_wasFovActive = fovActive;

        ApplyDistance(playable, settings, cfg.EnableDistance);
    }
}
