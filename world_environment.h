#pragma once
#include <Windows.h>
namespace WorldEnvironment
{
    struct Config { bool NightEnabled=false; float Strength=0.65f; float Coolness=0.60f; };
    Config GetConfig();
    void SetConfig(const Config& config);
    void Service(); // Present: posts setup only. No SDK reads/writes here.
    bool HandleWindowMessage(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam);
    void DrawSettings();
    bool PrepareUnload(); // Requests restore on owning window thread; refuses on pending lease.
    void CancelUnload(); // Re-enable dispatch if another module refuses unload.
    void Shutdown();      // Call only after successful PrepareUnload.
}
