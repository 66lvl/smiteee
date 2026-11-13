#pragma once
#include <Windows.h>
namespace TimeDilationExperiment
{
    struct Config { bool Enabled=false; float Multiplier=1.1f; };
    Config GetConfig();
    void SetConfig(const Config& value);
    void Disable();
    bool NeedsService();
    bool HasPendingRestore();
    // Called by the existing WorldEnvironment window-message dispatcher.
    // Window thread affinity is checked there; it is not proof of UE game-thread affinity.
    void TickOnWindowThread(HWND hwnd, bool suspending);
    void DrawPanel();
}
