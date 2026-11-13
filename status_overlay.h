#pragma once
#include <Windows.h>
namespace StatusOverlay
{
    struct Config { bool ShowFps=false; bool ShowPing=false; bool ShowSpectators=false; };
    Config GetConfig();
    void SetConfig(const Config& options);
    void Service(); // Only posts dispatcher setup; never reads game data from Present.
    bool HandleWindowMessage(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);
    void Draw(); // Values/strings only. No SDK work.
    void DrawSettings();
    void Shutdown();
}
