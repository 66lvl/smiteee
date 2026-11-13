#pragma once

#include <string>
#include "../imgui/imgui.h"
#include "../core/smite_sdk.h"

namespace AbilityIcons
{
    struct IconView
    {
        ImTextureID texId  = 0;
        int         width  = 0;
        int         height = 0;
        bool        valid  = false;
    };

    bool Initialize();
    void Shutdown();

    IconView GetAbilityIcon(const std::string& godCode, int slot1to4);
    IconView GetRelicIcon(const std::string& relicCode);
    std::string ResolveRelicCode(SDK::APawn* pawn);
    std::string ResolveGodCode(SDK::APawn* pawn);
    std::string CanonicalizeRelicCode(std::string code);
}
