#pragma once
#include <Windows.h>
namespace DeathCamera
{
    struct Config { bool Enabled=false; };
    Config GetConfig();
    void SetConfig(const Config& options);
    void Service();
    bool HandleWindowMessage(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp);
    void DrawSettings();
    bool PrepareUnload(); // Refuses unloading while an owned camera still needs restoration.
    void Shutdown();
}
