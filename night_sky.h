#pragma once
#include <Windows.h>
namespace NightSky
{
    bool NeedsService();
    bool HasPendingRestore();
    void Disable();
    void TickOnWindowThread(HWND hwnd,bool suspended);
    void DrawPanel();
}
