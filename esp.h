#pragma once
#include <Windows.h>
#include "../imgui/imgui.h"
#include <cstdint>

namespace ESP
{
    // ---------------------------------------------------------------
    //  Box drawing style
    // ---------------------------------------------------------------
    enum class BoxStyle : int { Full = 0, Corners, Lines2D };

    // ---------------------------------------------------------------
    //  Color struct (RGBA float)
    // ---------------------------------------------------------------
    struct Color4 { float r, g, b, a; };
    inline ImVec4 ToImVec4(const Color4& c) { return { c.r, c.g, c.b, c.a }; }
    inline ImU32  ToU32(const Color4& c)
    {
        return ImGui::ColorConvertFloat4ToU32({ c.r, c.g, c.b, c.a });
    }

    // ---------------------------------------------------------------
    //  Configuration
    // ---------------------------------------------------------------
    struct Config
    {
        bool Enabled          = false;

        // Entity toggles
        bool ShowEnemyGods    = true;
        bool ShowAllyGods     = false;
        bool ShowMinions       = false;
        bool ShowJungle        = false;
        bool ShowStructures    = false;
        bool ShowWards         = false;
        bool ShowEnemyWards    = true;
        bool ShowFriendlyWards = true;
        bool ShowWardRadius    = true;

        // Element toggles
        bool ShowBox          = true;
        bool ShowHealthBar    = true;
        bool ShowManaBar      = true;
        bool ShowName         = true;
        bool ShowDistance     = true;
        bool ShowLevel        = true;
        bool ShowSkeleton     = false;
        bool ShowSnaplines    = false;
        bool ShowShieldBar    = false;
        bool ShowCCIndicator  = true;
        bool ShowAbilityCooldowns = true;
        float AbilityCooldownScale = 1.0f; // ESP side-list size multiplier

        // Box style
        BoxStyle BoxMode      = BoxStyle::Corners;
        float    BoxThickness = 1.5f;
        float    CornerLength = 8.f;

        // Colors
        Color4 EnemyColor     = { 1.00f, 0.22f, 0.22f, 1.0f };
        Color4 AllyColor      = { 0.22f, 0.78f, 1.00f, 1.0f };
        Color4 MinionColor    = { 0.90f, 0.80f, 0.20f, 0.8f };
        Color4 JungleColor    = { 0.50f, 0.90f, 0.50f, 0.8f };
        Color4 WardColor      = { 1.00f, 0.22f, 0.22f, 1.0f };
        Color4 FriendlyWardColor = { 0.20f, 1.00f, 0.35f, 1.0f };
        Color4 StructureColor = { 0.60f, 0.60f, 0.60f, 1.0f };
        Color4 HealthColor    = { 0.15f, 0.85f, 0.25f, 1.0f };
        Color4 ManaColor      = { 0.25f, 0.50f, 1.00f, 1.0f };
        Color4 ShieldColor    = { 0.95f, 0.85f, 0.20f, 1.0f };
        Color4 SkeletonColor  = { 1.00f, 1.00f, 1.00f, 0.6f };
        Color4 TextColor      = { 1.00f, 1.00f, 1.00f, 1.0f };
        Color4 SnaplinesColor = { 1.00f, 1.00f, 1.00f, 0.3f };

        // Visibility / distances
        float MaxDistance      = 50000.f;  // UU
        float JungleMaxDistance = 25000.f; // UU — jungle camp ESP only
        bool  VisibleCheck     = false;    // Skip not-rendered chars

        // Text size
        float NameFontSize    = 1.0f;
    };

    extern Config cfg;

    void Reset(); // discard ESP value caches when the world/session changes
    void Tick(); // called once per frame from the draw callback
}
