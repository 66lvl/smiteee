#pragma once
#include <Windows.h>
#include <cstdint>

namespace Aimbot
{
    enum class TargetMode : int
    {
        ClosestToCrosshair = 0,
        LowestHealth
    };

    enum class HoldBindType : int
    {
        Keyboard = 0,
        Gamepad  = 1
    };

    struct Config
    {
        bool Enabled = false;

        HoldBindType HoldBind = HoldBindType::Keyboard;
        int HoldKey               = VK_XBUTTON1;
        int HoldPadDevice         = 0;
        int HoldPadButton         = 0;

        HoldBindType ToggleBind = HoldBindType::Keyboard;
        int ToggleKey               = 0; // 0 = unbound
        int TogglePadDevice         = 0;
        int TogglePadButton         = 0;

        HoldBindType NoPredBind = HoldBindType::Keyboard;
        int NoPredKey               = VK_XBUTTON2;
        int NoPredPadDevice         = 0;
        int NoPredPadButton         = 0;

        TargetMode TargetPri = TargetMode::ClosestToCrosshair;

        float FovDeg            = 30.f;
        float Smoothing         = 5.f;
        float RangeMeters       = 40.f;
        float ProjectileSpeed   = 1700.f;
        float LeadScale         = 1.f;
        float DynamicSmoothScale = 0.5f;

        bool TeamCheck          = true;
        bool HoldMode           = true;
        bool HardLock           = false;
        bool UseRecommendedSpeed = false;
        bool ShowRecommendedSpeed = true;
        bool ShowFovCircle      = false;
        bool ShowImpactOverlay  = true;
        bool ShowTargetInfo     = false;
    };

    extern Config cfg;

    void Tick();
    void ResetState();
    bool IsToggleLatched();
}
