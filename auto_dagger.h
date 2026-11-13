#pragma once
namespace AutoDagger
{
    struct Config
    {
        bool Enabled = false;
        int RetryIntervalMs = 100;
    };
    extern Config cfg;
    void Tick(bool playable, bool menuOpen);
    void Reset();
    const char* Status();
    void PrintInventory();
}
