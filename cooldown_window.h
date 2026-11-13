#pragma once
#include <string>

namespace CooldownWindow
{
    struct Config
    {
        bool Enabled     = false;
        bool ShowEnemies = true;
        bool ShowTeam    = false;
        bool ShowSelf    = true;
        bool SplitHubs   = false;
        bool TransparentBackground = false;
        bool WidgetMode  = false;
        bool WidgetVerticalLayout = false;
        float HubTileSize = 38.f;
        float WidgetGodNameSize = 15.f;
        bool ShowGodName = true;
        bool ShowDistance = true;
        bool ShowAbility1 = true;
        bool ShowAbility2 = true;
        bool ShowAbility3 = true;
        bool ShowAbility4 = true;
        bool ShowRelic = true;
        float CombinedPosX = -1.f;
        float CombinedPosY = -1.f;
        float EnemyHubPosX = -1.f;
        float EnemyHubPosY = -1.f;
        float TeamHubPosX  = -1.f;
        float TeamHubPosY  = -1.f;
        std::string WidgetLayout;
    };

    void ResetWidgetLayoutCache();
    void CaptureWidgetLayout();

    extern Config cfg;

    void Tick();
}
