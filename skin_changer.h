#pragma once

#include <string>
#include <vector>
#include "../core/smite_sdk.h"

namespace SkinChanger
{
    struct SkinEntry
    {
        std::string       Label;
        SDK::UHWSkinItem* Item = nullptr;
    };

    void ResetSession();
    void Scan();
    void ApplySelected(int index);
    void TickPending();
    bool HasScanned();
    void DrawMenu();

    const std::string& CurrentGod();
    const std::vector<SkinEntry>& Skins();
    const char* LastStatus();
}
