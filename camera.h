#pragma once

namespace Camera
{
    struct Config
    {
        bool  EnableFov      = false;
        float FovValue       = 103.f;
        bool  EnableDistance = false;
        float DistanceValue  = 1200.f;
        bool  NoArmCollision = false;
    };

    extern Config cfg;

    void Reset();
    void Tick();
}
