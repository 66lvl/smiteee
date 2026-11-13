#pragma once
#include <Windows.h>
#include <string>
namespace AutoPick
{
    struct Config
    {
        bool Enabled = false;
        int Mode = 0; // 0 Random, 1 Specific
        float TriggerSeconds = 1.f;
        std::string GodKey;
    };
    Config GetConfig();
    void SetConfig(const Config& value);
    std::string Status();
    void Service(); // Posts setup only. No SDK calls/selection from Present.
    bool HandleWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void DrawSettings();
    void Shutdown();
}
