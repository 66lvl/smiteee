#pragma once

namespace MapHack
{
    struct Config
    {
        bool Enabled           = false;
        bool ShowEnemyGods     = true;
        bool ShowEnemyWards    = true;
        bool ShowEnemyFamiliars = true;
        bool ShowJungle        = false;
    };

    extern Config cfg;

    void Reset();
    void Tick();
}
