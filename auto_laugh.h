#pragma once
#include <Windows.h>
namespace AutoLaugh
{
    struct Config { bool Enabled=false; int HoldKey=0; float IntervalSeconds=0.1f; };
    Config GetConfig();
    void SetConfig(const Config& value);
    void Service(bool menuOrTextInput);
    bool HandleWindowMessage(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp);
    void Shutdown();
    void DrawSettings();
}
