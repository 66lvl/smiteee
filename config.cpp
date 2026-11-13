#include "time_dilation.h"
#include "world_environment.h"
#include "crosshair.h"
#include "auto_laugh.h"
#include "death_camera.h"
#include "status_overlay.h"
#include "esp.h"
#include "aimbot.h"
#include "maphack.h"
#include "camera.h"
#include "cooldown_window.h"
#include "config.h"
#include "auto_dagger.h"
#include "auto_pick.h"
#include "../core/gamepad_input.h"
#include "../ui/theme.h"
#include <Windows.h>
#include <ShlObj.h>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdio>

#pragma comment(lib, "Shell32.lib")

// Definitions for the declarations in config.h.
bool silentAimExperimentEnabled = false;
int silentAimExperimentType = 0;

/*  Minimal hand-rolled JSON serializer/deserializer.
    Avoids dependency on nlohmann or rapidjson for portability.  */

namespace Config
{
    static std::string GetConfigPath()
    {
        char appData[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
            return "";
        std::string dir = std::string(appData) + "\\HemingwayTrainer\\";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "config.json";
    }

    // ---------------------------------------------------------------
    //  Minimal JSON writer helpers
    // ---------------------------------------------------------------
    struct Writer
    {
        std::ostringstream ss;
        int indent = 0;

        void Ind() { for (int i = 0; i < indent; i++) ss << "  "; }

        void Bool (const char* k, bool v)         { Ind(); ss << "\"" << k << "\": " << (v?"true":"false") << ",\n"; }
        void Int  (const char* k, int v)           { Ind(); ss << "\"" << k << "\": " << v << ",\n"; }
        void Float(const char* k, float v)         { Ind(); ss << "\"" << k << "\": " << v << ",\n"; }
        void Color(const char* k, const ESP::Color4& c)
        {
            Ind();
            ss << "\"" << k << "\": [" << c.r << "," << c.g << "," << c.b << "," << c.a << "],\n";
        }
        void String(const char* k, const std::string& v)
        {
            Ind();
            ss << "\"" << k << "\": \"";
            for (char c : v)
            {
                if (c == '\\' || c == '\"')
                    ss << '\\';
                ss << c;
            }
            ss << "\",\n";
        }

        std::string Str()
        {
            // Writer fields include a separator. Remove trailing separators outside strings.
            const std::string raw=ss.str();
            std::string result; result.reserve(raw.size());
            bool inString=false, escaped=false;
            for(size_t i=0;i<raw.size();++i)
            {
                const char ch=raw[i];
                if(inString)
                {
                    result.push_back(ch);
                    if(escaped) escaped=false;
                    else if(ch=='\\') escaped=true;
                    else if(ch=='"') inString=false;
                    continue;
                }
                if(ch=='"') inString=true;
                if(ch==',')
                {
                    size_t next=i+1;
                    while(next<raw.size()&&isspace(static_cast<unsigned char>(raw[next])))++next;
                    if(next<raw.size()&&(raw[next]=='}'||raw[next]==']'))continue;
                }
                result.push_back(ch);
            }
            return result;
        }
    };

    // ---------------------------------------------------------------
    //  Minimal JSON reader helpers
    // ---------------------------------------------------------------
    static std::string ReadFile(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) return "";
        std::ostringstream ss;
        ss << f.rdbuf();
        if (f.bad()) return "";
        return ss.str();
    }

    static bool FindKey(const std::string& json, const std::string& key, size_t& pos)
    {
        std::string search = "\"" + key + "\"";
        pos = json.find(search);
        return pos != std::string::npos;
    }

    static bool ReadBool(const std::string& json, const std::string& key, bool& out)
    {
        size_t pos;
        if (!FindKey(json, key, pos)) return false;
        size_t vpos = json.find(":", pos) + 1;
        while (vpos < json.size() && isspace((unsigned char)json[vpos])) vpos++;
        if (vpos < json.size())
        {
            out = (json.substr(vpos, 4) == "true");
            return true;
        }
        return false;
    }

    static bool ReadInt(const std::string& json, const std::string& key, int& out)
    {
        size_t pos;
        if (!FindKey(json, key, pos)) return false;
        size_t vpos = json.find(":", pos) + 1;
        while (vpos < json.size() && isspace((unsigned char)json[vpos])) vpos++;
        if (sscanf_s(json.c_str() + vpos, "%d", &out) == 1) return true;
        return false;
    }

    static bool ReadFloat(const std::string& json, const std::string& key, float& out)
    {
        size_t pos;
        if (!FindKey(json, key, pos)) return false;
        size_t vpos = json.find(":", pos) + 1;
        while (vpos < json.size() && isspace((unsigned char)json[vpos])) vpos++;
        if (sscanf_s(json.c_str() + vpos, "%f", &out) == 1) return true;
        return false;
    }

    static bool ReadString(const std::string& json, const std::string& key, std::string& out)
    {
        size_t pos;
        if (!FindKey(json, key, pos))
            return false;
        size_t q0 = json.find('\"', json.find(':', pos) + 1);
        if (q0 == std::string::npos)
            return false;
        ++q0;
        out.clear();
        for (size_t i = q0; i < json.size(); ++i)
        {
            if (json[i] == '\\' && i + 1 < json.size())
            {
                out.push_back(json[++i]);
                continue;
            }
            if (json[i] == '\"')
                return true;
            out.push_back(json[i]);
        }
        return false;
    }

    static bool ReadColor(const std::string& json, const std::string& key, ESP::Color4& out)
    {
        size_t pos;
        if (!FindKey(json, key, pos)) return false;
        size_t vpos = json.find("[", pos);
        if (vpos == std::string::npos) return false;
        if (sscanf_s(json.c_str() + vpos + 1, "%f,%f,%f,%f",
                     &out.r, &out.g, &out.b, &out.a) == 4) return true;
        return false;
    }

    // ---------------------------------------------------------------
    //  Save config
    // ---------------------------------------------------------------
    bool Save()
    {
        CooldownWindow::CaptureWidgetLayout();

        Writer w;
        w.ss << "{\n";
        w.indent = 1;

        // ESP
        w.ss << "  \"esp\": {\n";
        w.indent = 2;
        w.Bool("enabled",       ESP::cfg.Enabled);
        w.Bool("show_enemy",    ESP::cfg.ShowEnemyGods);
        w.Bool("show_ally",     ESP::cfg.ShowAllyGods);
        w.Bool("show_minions",  ESP::cfg.ShowMinions);
        w.Bool("show_jungle",   ESP::cfg.ShowJungle);
        w.Bool("show_structs",  ESP::cfg.ShowStructures);
        w.Bool("show_wards",    ESP::cfg.ShowWards);
        w.Bool("show_enemy_wards", ESP::cfg.ShowEnemyWards);
        w.Bool("show_friendly_wards", ESP::cfg.ShowFriendlyWards);
        w.Bool("show_ward_radius", ESP::cfg.ShowWardRadius);
        w.Bool("show_box",      ESP::cfg.ShowBox);
        w.Bool("show_hp",       ESP::cfg.ShowHealthBar);
        w.Bool("show_mana",     ESP::cfg.ShowManaBar);
        w.Bool("show_shield",   ESP::cfg.ShowShieldBar);
        w.Bool("show_name",     ESP::cfg.ShowName);
        w.Bool("show_dist",     ESP::cfg.ShowDistance);
        w.Bool("show_level",    ESP::cfg.ShowLevel);
        w.Bool("show_skel",     ESP::cfg.ShowSkeleton);
        w.Bool("show_snap",     ESP::cfg.ShowSnaplines);
        w.Bool("show_cc",       ESP::cfg.ShowCCIndicator);
        w.Bool("show_cds",      ESP::cfg.ShowAbilityCooldowns);
        w.Bool("cd_window",     CooldownWindow::cfg.Enabled);
        w.Bool("cd_window_enemies", CooldownWindow::cfg.ShowEnemies);
        w.Bool("cd_window_team",    CooldownWindow::cfg.ShowTeam);
        w.Bool("cd_window_self",    CooldownWindow::cfg.ShowSelf);
        w.Bool("cd_window_split",   CooldownWindow::cfg.SplitHubs);
        w.Bool("cd_window_transparent", CooldownWindow::cfg.TransparentBackground);
        w.Bool("cd_window_widgets", CooldownWindow::cfg.WidgetMode);
        w.Bool("cd_window_widget_vertical", CooldownWindow::cfg.WidgetVerticalLayout);
        w.Float("cd_hub_tile_size", CooldownWindow::cfg.HubTileSize);
        w.Float("cd_hub_widget_name_size", CooldownWindow::cfg.WidgetGodNameSize);
        w.Bool("cd_hub_show_name", CooldownWindow::cfg.ShowGodName);
        w.Bool("cd_hub_show_dist", CooldownWindow::cfg.ShowDistance);
        w.Bool("cd_hub_show_a1", CooldownWindow::cfg.ShowAbility1);
        w.Bool("cd_hub_show_a2", CooldownWindow::cfg.ShowAbility2);
        w.Bool("cd_hub_show_a3", CooldownWindow::cfg.ShowAbility3);
        w.Bool("cd_hub_show_a4", CooldownWindow::cfg.ShowAbility4);
        w.Bool("cd_hub_show_relic", CooldownWindow::cfg.ShowRelic);
        w.Float("cd_hub_combined_x", CooldownWindow::cfg.CombinedPosX);
        w.Float("cd_hub_combined_y", CooldownWindow::cfg.CombinedPosY);
        w.Float("cd_hub_enemy_x", CooldownWindow::cfg.EnemyHubPosX);
        w.Float("cd_hub_enemy_y", CooldownWindow::cfg.EnemyHubPosY);
        w.Float("cd_hub_team_x", CooldownWindow::cfg.TeamHubPosX);
        w.Float("cd_hub_team_y", CooldownWindow::cfg.TeamHubPosY);
        w.String("cd_hub_widgets", CooldownWindow::cfg.WidgetLayout);
        w.Float("cd_esp_scale", ESP::cfg.AbilityCooldownScale);
        w.Int ("box_style",     (int)ESP::cfg.BoxMode);
        w.Float("box_thick",    ESP::cfg.BoxThickness);
        w.Float("corner_len",   ESP::cfg.CornerLength);
        w.Float("max_dist",     ESP::cfg.MaxDistance);
        w.Float("jungle_dist",  ESP::cfg.JungleMaxDistance);
        w.Bool ("visible_check", ESP::cfg.VisibleCheck);
        w.Color("col_enemy",    ESP::cfg.EnemyColor);
        w.Color("col_ally",     ESP::cfg.AllyColor);
        w.Color("col_minion",   ESP::cfg.MinionColor);
        w.Color("col_jungle",   ESP::cfg.JungleColor);
        w.Color("col_ward",     ESP::cfg.WardColor);
        w.Color("col_friendly_ward", ESP::cfg.FriendlyWardColor);
        w.Color("col_struct",   ESP::cfg.StructureColor);
        w.Color("col_hp",       ESP::cfg.HealthColor);
        w.Color("col_mana",     ESP::cfg.ManaColor);
        w.Color("col_shield",   ESP::cfg.ShieldColor);
        w.Color("col_skeleton", ESP::cfg.SkeletonColor);
        w.Color("col_snaplines", ESP::cfg.SnaplinesColor);
        w.Color("col_text",     ESP::cfg.TextColor);
        w.indent = 1;
        w.ss << "  },\n";

        // Aimbot
        w.ss << "  \"aimbot\": {\n";
        w.indent = 2;
        w.Bool ("enabled",            Aimbot::cfg.Enabled);
        w.Int  ("hold_bind_type",     (int)Aimbot::cfg.HoldBind);
        w.Int  ("hold_key",           Aimbot::cfg.HoldKey);
        w.Int  ("hold_pad_device",    Aimbot::cfg.HoldPadDevice);
        w.Int  ("hold_pad_button",    Aimbot::cfg.HoldPadButton);
        w.Int  ("toggle_bind_type",   (int)Aimbot::cfg.ToggleBind);
        w.Int  ("toggle_key",         Aimbot::cfg.ToggleKey);
        w.Int  ("toggle_pad_device",  Aimbot::cfg.TogglePadDevice);
        w.Int  ("toggle_pad_button",  Aimbot::cfg.TogglePadButton);
        w.Int  ("nopred_bind_type",   (int)Aimbot::cfg.NoPredBind);
        w.Int  ("nopred_key",         Aimbot::cfg.NoPredKey);
        w.Int  ("nopred_pad_device",  Aimbot::cfg.NoPredPadDevice);
        w.Int  ("nopred_pad_button",  Aimbot::cfg.NoPredPadButton);
        w.Int  ("target_mode",        (int)Aimbot::cfg.TargetPri);
        w.Float("fov_deg",            Aimbot::cfg.FovDeg);
        w.Float("smoothing",          Aimbot::cfg.Smoothing);
        w.Float("range_m",            Aimbot::cfg.RangeMeters);
        w.Float("proj_speed",         Aimbot::cfg.ProjectileSpeed);
        w.Float("lead_scale",         Aimbot::cfg.LeadScale);
        w.Float("dynamic_smooth",     Aimbot::cfg.DynamicSmoothScale);
        w.Bool ("team_check",         Aimbot::cfg.TeamCheck);
        w.Bool ("hard_lock",          Aimbot::cfg.HardLock);
        w.Bool ("use_recommended",    Aimbot::cfg.UseRecommendedSpeed);
        w.Bool ("show_recommended",   Aimbot::cfg.ShowRecommendedSpeed);
        w.Bool ("show_fov",           Aimbot::cfg.ShowFovCircle);
        w.Bool ("show_impact",        Aimbot::cfg.ShowImpactOverlay);
        w.Bool ("show_target_info",   Aimbot::cfg.ShowTargetInfo);
        w.indent = 1;
        w.ss << "  },\n";

        // --- Silent Aim Experiment (new experimental feature) ---
        w.ss << "  \"silent_aim_experiment\": {\n";
        w.indent = 2;
        w.Bool("enabled", silentAimExperimentEnabled);
        w.Int("type", silentAimExperimentType);
        w.indent = 1;
        w.ss << "  },\n";

        const auto crosshair=Crosshair::GetConfig();
        w.ss << "  \"crosshair\": {\n";
        w.indent=2;
        w.Bool("enabled",crosshair.Enabled);w.Int("shape",crosshair.Style);
        w.Bool("hide_menu",crosshair.HideInMenu);w.Bool("require_context",crosshair.RequireGameContext);w.Bool("resolution_scale",crosshair.ScaleWithResolution);
        w.Float("length",crosshair.Length);w.Float("thickness",crosshair.Thickness);w.Float("gap",crosshair.Gap);
        w.Bool("center_dot",crosshair.CenterDot);w.Float("dot_size",crosshair.DotSize);w.Float("radius",crosshair.Radius);
        w.Bool("outline",crosshair.Outline);w.Float("outline_thickness",crosshair.OutlineThickness);w.Float("outline_opacity",crosshair.OutlineOpacity);
        w.Color("color",ESP::Color4{crosshair.Red,crosshair.Green,crosshair.Blue,crosshair.Opacity});
        w.indent=1;w.ss << "  },\n";

        const auto environment=WorldEnvironment::GetConfig();
        w.ss << "  \"world_environment\": {\n";
        w.indent=2;
        w.Bool("night_enabled",environment.NightEnabled);
        w.Float("night_strength",environment.Strength);
        w.Float("coolness",environment.Coolness);
        w.indent=1;w.ss << "  },\n";

        // MapHack
        w.ss << "  \"maphack\": {\n";
        w.indent = 2;
        w.Bool("enabled",    MapHack::cfg.Enabled);
        w.Bool("enemy_gods", MapHack::cfg.ShowEnemyGods);
        w.Bool("wards",      MapHack::cfg.ShowEnemyWards);
        w.Bool("familiars",  MapHack::cfg.ShowEnemyFamiliars);
        w.Bool("jungle",     MapHack::cfg.ShowJungle);
        w.indent = 1;
        w.ss << "  },\n";

        w.ss << "  \"camera\": {\n";
        w.indent = 2;
        w.Bool("enable_fov",       Camera::cfg.EnableFov);
        w.Float("fov_value",       Camera::cfg.FovValue);
        w.Bool("enable_distance",  Camera::cfg.EnableDistance);
        w.Float("distance_value", Camera::cfg.DistanceValue);
        w.Bool("no_collision",     Camera::cfg.NoArmCollision);
        w.indent = 1;
        w.ss << "  },\n";

        w.ss << "  \"menu\": {\n";
        w.indent = 2;
        w.Int("menu_design_version", 5);
        const auto hud=StatusOverlay::GetConfig();
        w.Bool("death_camera_enabled",DeathCamera::GetConfig().Enabled);
        w.Bool("show_fps",hud.ShowFps);
        w.Bool("show_ping",hud.ShowPing);
        w.Bool("show_spectators",hud.ShowSpectators);
        const auto laugh=AutoLaugh::GetConfig();
        w.Bool("auto_laugh_enabled",laugh.Enabled);
        w.Int("auto_laugh_key",laugh.HoldKey);
        w.Float("auto_laugh_interval",laugh.IntervalSeconds);
        const auto autoPick = AutoPick::GetConfig();
        w.Bool("auto_pick_enabled", autoPick.Enabled);
        w.Int("auto_pick_mode", autoPick.Mode);
        w.Float("auto_pick_seconds", autoPick.TriggerSeconds);
        w.String("auto_pick_god", autoPick.GodKey);
        w.Bool("auto_dagger", AutoDagger::cfg.Enabled);
        w.Int("auto_dagger_retry_ms", AutoDagger::cfg.RetryIntervalMs);
        w.Int("theme_preset", UITheme::appearance.Preset);
        w.Color("theme_accent", UITheme::appearance.Accent);
        w.Color("theme_background", UITheme::appearance.Background);
        w.Color("theme_surface", UITheme::appearance.Surface);
        w.Color("theme_sidebar", UITheme::appearance.Sidebar);
        w.Color("theme_text", UITheme::appearance.Text);
        w.Float("theme_opacity", UITheme::appearance.Opacity);
        w.Float("theme_rounding", UITheme::appearance.Rounding);
        w.Float("theme_border", UITheme::appearance.BorderStrength);
        UITheme::SanitizeComfort();
        w.Float("menu_scale", UITheme::comfort.Scale);
        w.Bool("menu_tooltips", UITheme::comfort.Tooltips);
        w.Bool("particles",        UITheme::menuFx.ParticlesEnabled);
        w.Int ("particle_mode",    UITheme::menuFx.ParticleMode);
        w.Float("particle_density", UITheme::menuFx.ParticleDensity);
        w.Float("particle_speed",   UITheme::menuFx.ParticleSpeed);
        w.indent = 1;
        w.ss << "  }\n";

        w.ss << "}\n";

        std::string path = GetConfigPath();
        std::ofstream f(path);
        if (path.empty() || !f) return false;
        f << w.Str();
        f.flush();
        if (!f) return false;
        f.close();
        return !f.fail();
    }

    // ---------------------------------------------------------------
    //  Extract section braces helper
    // ---------------------------------------------------------------
    static std::string ExtractSection(const std::string& json, const std::string& sectionKey)
    {
        size_t pos = json.find("\"" + sectionKey + "\":");
        if (pos == std::string::npos) return "";
        size_t start = json.find("{", pos);
        if (start == std::string::npos) return "";
        
        int depth = 0;
        for (size_t i = start; i < json.size(); i++)
        {
            if (json[i] == '{') depth++;
            else if (json[i] == '}')
            {
                depth--;
                if (depth == 0)
                {
                    return json.substr(start, i - start + 1);
                }
            }
        }
        return "";
    }

    // ---------------------------------------------------------------
    //  Load config
    // ---------------------------------------------------------------
    bool Load()
    {
        TimeDilationExperiment::Disable(); // Never carry an experimental time lease across profile loads.

        // Silent Aim Experiment Toggle (new experimental feature)
        silentAimExperimentEnabled = false;
        silentAimExperimentType = 0;

        std::string path = GetConfigPath();
        std::string json = ReadFile(path);
        // A failed Load must not reset settings or overwrite the user's file.
        if (path.empty() || json.empty()) return false;
        const size_t first = json.find_first_not_of(" \t\r\n");
        const size_t last = json.find_last_not_of(" \t\r\n");
        if (first == std::string::npos || json[first] != '{' || json[last] != '}') return false;
        bool recognized = false;
        for (const char* section : {"esp", "aimbot", "maphack", "camera", "menu", "world_environment", "crosshair"})
            recognized = recognized || !ExtractSection(json, section).empty();
        if (!recognized) return false;

        // Restore the experiment settings from their own section.
        const std::string silentAimExpJson = ExtractSection(json, "silent_aim_experiment");
        if (!silentAimExpJson.empty())
        {
            ReadBool(silentAimExpJson, "enabled", silentAimExperimentEnabled);
            ReadInt(silentAimExpJson, "type", silentAimExperimentType);
        }

        // --- ESP ---
        std::string espJson = ExtractSection(json, "esp");
        if (!espJson.empty())
        {
            ReadBool (espJson, "enabled",       ESP::cfg.Enabled);
            ReadBool (espJson, "show_enemy",    ESP::cfg.ShowEnemyGods);
            ReadBool (espJson, "show_ally",     ESP::cfg.ShowAllyGods);
            ReadBool (espJson, "show_minions",  ESP::cfg.ShowMinions);
            ReadBool (espJson, "show_jungle",   ESP::cfg.ShowJungle);
            ReadBool (espJson, "show_structs",  ESP::cfg.ShowStructures);
            ReadBool (espJson, "show_wards",    ESP::cfg.ShowWards);
            ReadBool (espJson, "show_enemy_wards", ESP::cfg.ShowEnemyWards);
            ReadBool (espJson, "show_friendly_wards", ESP::cfg.ShowFriendlyWards);
            ReadBool (espJson, "show_ward_radius", ESP::cfg.ShowWardRadius);
            ReadBool (espJson, "show_box",      ESP::cfg.ShowBox);
            ReadBool (espJson, "show_hp",       ESP::cfg.ShowHealthBar);
            ReadBool (espJson, "show_mana",     ESP::cfg.ShowManaBar);
            ReadBool (espJson, "show_shield",   ESP::cfg.ShowShieldBar);
            ReadBool (espJson, "show_name",     ESP::cfg.ShowName);
            ReadBool (espJson, "show_dist",     ESP::cfg.ShowDistance);
            ReadBool (espJson, "show_level",    ESP::cfg.ShowLevel);
            ReadBool (espJson, "show_skel",     ESP::cfg.ShowSkeleton);
            ReadBool (espJson, "show_snap",     ESP::cfg.ShowSnaplines);
            ReadBool (espJson, "show_cc",       ESP::cfg.ShowCCIndicator);
            ReadBool (espJson, "show_cds",      ESP::cfg.ShowAbilityCooldowns);
            ReadFloat(espJson, "cd_esp_scale",  ESP::cfg.AbilityCooldownScale);
            ReadBool (espJson, "cd_window",     CooldownWindow::cfg.Enabled);
            ReadBool (espJson, "cd_window_enemies", CooldownWindow::cfg.ShowEnemies);
            ReadBool (espJson, "cd_window_team",    CooldownWindow::cfg.ShowTeam);
            ReadBool (espJson, "cd_window_self",    CooldownWindow::cfg.ShowSelf);
            ReadBool (espJson, "cd_window_split",   CooldownWindow::cfg.SplitHubs);
            ReadBool (espJson, "cd_window_transparent", CooldownWindow::cfg.TransparentBackground);
            ReadBool (espJson, "cd_window_widgets", CooldownWindow::cfg.WidgetMode);
            ReadBool (espJson, "cd_window_widget_vertical", CooldownWindow::cfg.WidgetVerticalLayout);
            ReadFloat(espJson, "cd_hub_tile_size", CooldownWindow::cfg.HubTileSize);
            ReadFloat(espJson, "cd_hub_widget_name_size", CooldownWindow::cfg.WidgetGodNameSize);
            ReadBool (espJson, "cd_hub_show_name", CooldownWindow::cfg.ShowGodName);
            ReadBool (espJson, "cd_hub_show_dist", CooldownWindow::cfg.ShowDistance);
            ReadBool (espJson, "cd_hub_show_a1", CooldownWindow::cfg.ShowAbility1);
            ReadBool (espJson, "cd_hub_show_a2", CooldownWindow::cfg.ShowAbility2);
            ReadBool (espJson, "cd_hub_show_a3", CooldownWindow::cfg.ShowAbility3);
            ReadBool (espJson, "cd_hub_show_a4", CooldownWindow::cfg.ShowAbility4);
            ReadBool (espJson, "cd_hub_show_relic", CooldownWindow::cfg.ShowRelic);
            ReadFloat(espJson, "cd_hub_combined_x", CooldownWindow::cfg.CombinedPosX);
            ReadFloat(espJson, "cd_hub_combined_y", CooldownWindow::cfg.CombinedPosY);
            ReadFloat(espJson, "cd_hub_enemy_x", CooldownWindow::cfg.EnemyHubPosX);
            ReadFloat(espJson, "cd_hub_enemy_y", CooldownWindow::cfg.EnemyHubPosY);
            ReadFloat(espJson, "cd_hub_team_x", CooldownWindow::cfg.TeamHubPosX);
            ReadFloat(espJson, "cd_hub_team_y", CooldownWindow::cfg.TeamHubPosY);
            ReadString(espJson, "cd_hub_widgets", CooldownWindow::cfg.WidgetLayout);
            CooldownWindow::ResetWidgetLayoutCache();
            int boxStyle = (int)ESP::cfg.BoxMode;
            ReadInt  (espJson, "box_style",     boxStyle); ESP::cfg.BoxMode = (ESP::BoxStyle)boxStyle;
            ReadFloat(espJson, "box_thick",     ESP::cfg.BoxThickness);
            ReadFloat(espJson, "corner_len",    ESP::cfg.CornerLength);
            ReadFloat(espJson, "max_dist",      ESP::cfg.MaxDistance);
            ReadFloat(espJson, "jungle_dist",   ESP::cfg.JungleMaxDistance);
            ReadBool (espJson, "visible_check", ESP::cfg.VisibleCheck);
            ReadColor(espJson, "col_enemy",     ESP::cfg.EnemyColor);
            ReadColor(espJson, "col_ally",      ESP::cfg.AllyColor);
            ReadColor(espJson, "col_minion",    ESP::cfg.MinionColor);
            ReadColor(espJson, "col_jungle",    ESP::cfg.JungleColor);
            ReadColor(espJson, "col_ward",      ESP::cfg.WardColor);
            ReadColor(espJson, "col_friendly_ward", ESP::cfg.FriendlyWardColor);
            ReadColor(espJson, "col_struct",    ESP::cfg.StructureColor);
            ReadColor(espJson, "col_hp",        ESP::cfg.HealthColor);
            ReadColor(espJson, "col_mana",      ESP::cfg.ManaColor);
            ReadColor(espJson, "col_shield",    ESP::cfg.ShieldColor);
            ReadColor(espJson, "col_skeleton",  ESP::cfg.SkeletonColor);
            ReadColor(espJson, "col_snaplines", ESP::cfg.SnaplinesColor);
            ReadColor(espJson, "col_text",      ESP::cfg.TextColor);
        }

        // --- Aimbot ---
        std::string aimJson = ExtractSection(json, "aimbot");
        if (!aimJson.empty())
        {
            ReadBool (aimJson, "enabled",          Aimbot::cfg.Enabled);
            int bindType = (int)Aimbot::cfg.HoldBind;
            ReadInt  (aimJson, "hold_bind_type",   bindType);
            Aimbot::cfg.HoldBind = (Aimbot::HoldBindType)bindType;
            ReadInt  (aimJson, "hold_key",         Aimbot::cfg.HoldKey);
            ReadInt  (aimJson, "hold_pad_device",  Aimbot::cfg.HoldPadDevice);
            ReadInt  (aimJson, "hold_pad_button",  Aimbot::cfg.HoldPadButton);
            int toggleBindType = (int)Aimbot::cfg.ToggleBind;
            ReadInt  (aimJson, "toggle_bind_type",  toggleBindType);
            Aimbot::cfg.ToggleBind = (Aimbot::HoldBindType)toggleBindType;
            ReadInt  (aimJson, "toggle_key",        Aimbot::cfg.ToggleKey);
            ReadInt  (aimJson, "toggle_pad_device", Aimbot::cfg.TogglePadDevice);
            ReadInt  (aimJson, "toggle_pad_button", Aimbot::cfg.TogglePadButton);
            int noPredBindType = (int)Aimbot::cfg.NoPredBind;
            ReadInt  (aimJson, "nopred_bind_type",  noPredBindType);
            Aimbot::cfg.NoPredBind = (Aimbot::HoldBindType)noPredBindType;
            ReadInt  (aimJson, "nopred_key",         Aimbot::cfg.NoPredKey);
            ReadInt  (aimJson, "nopred_pad_device",  Aimbot::cfg.NoPredPadDevice);
            ReadInt  (aimJson, "nopred_pad_button",  Aimbot::cfg.NoPredPadButton);

            // Legacy controller settings
            bool legacyUseController = false;
            if (ReadBool(aimJson, "use_controller", legacyUseController) && legacyUseController)
            {
                Aimbot::cfg.HoldBind = Aimbot::HoldBindType::Gamepad;
                int legacyBtn = Aimbot::cfg.HoldPadButton;
                int legacyIdx = 0;
                ReadInt(aimJson, "controller_button", legacyBtn);
                ReadInt(aimJson, "controller_index", legacyIdx);
                Aimbot::cfg.HoldPadButton = legacyBtn;
                Aimbot::cfg.HoldPadDevice = GamepadInput::FindDevice(
                    GamepadInput::Backend::XInput, legacyIdx);
            }

            int tMode = (int)Aimbot::cfg.TargetPri;
            ReadInt  (aimJson, "target_mode",      tMode);
            Aimbot::cfg.TargetPri = (Aimbot::TargetMode)tMode;
            ReadFloat(aimJson, "fov_deg",           Aimbot::cfg.FovDeg);
            ReadFloat(aimJson, "smoothing",         Aimbot::cfg.Smoothing);
            ReadFloat(aimJson, "range_m",           Aimbot::cfg.RangeMeters);
            ReadFloat(aimJson, "proj_speed",        Aimbot::cfg.ProjectileSpeed);
            ReadFloat(aimJson, "lead_scale",        Aimbot::cfg.LeadScale);
            ReadFloat(aimJson, "dynamic_smooth",    Aimbot::cfg.DynamicSmoothScale);
            ReadBool (aimJson, "team_check",        Aimbot::cfg.TeamCheck);
            ReadBool (aimJson, "hard_lock",         Aimbot::cfg.HardLock);
            ReadBool (aimJson, "use_recommended",   Aimbot::cfg.UseRecommendedSpeed);
            ReadBool (aimJson, "show_recommended",  Aimbot::cfg.ShowRecommendedSpeed);
            ReadBool (aimJson, "show_fov",          Aimbot::cfg.ShowFovCircle);
            ReadBool (aimJson, "show_impact",       Aimbot::cfg.ShowImpactOverlay);
            ReadBool (aimJson, "show_target_info",  Aimbot::cfg.ShowTargetInfo);
            // Legacy keys
            ReadFloat(aimJson, "fov_radius",       Aimbot::cfg.FovDeg);
            ReadBool (aimJson, "enemy_only",        Aimbot::cfg.TeamCheck);
        }

        Crosshair::Config crosshair{};
        const auto crosshairJson=ExtractSection(json,"crosshair");
        if(!crosshairJson.empty())
        {
            ReadBool(crosshairJson,"enabled",crosshair.Enabled);ReadInt(crosshairJson,"shape",crosshair.Style);
            ReadBool(crosshairJson,"hide_menu",crosshair.HideInMenu);ReadBool(crosshairJson,"require_context",crosshair.RequireGameContext);ReadBool(crosshairJson,"resolution_scale",crosshair.ScaleWithResolution);
            ReadFloat(crosshairJson,"length",crosshair.Length);ReadFloat(crosshairJson,"thickness",crosshair.Thickness);ReadFloat(crosshairJson,"gap",crosshair.Gap);
            ReadBool(crosshairJson,"center_dot",crosshair.CenterDot);ReadFloat(crosshairJson,"dot_size",crosshair.DotSize);ReadFloat(crosshairJson,"radius",crosshair.Radius);
            ReadBool(crosshairJson,"outline",crosshair.Outline);ReadFloat(crosshairJson,"outline_thickness",crosshair.OutlineThickness);ReadFloat(crosshairJson,"outline_opacity",crosshair.OutlineOpacity);
            ESP::Color4 color{crosshair.Red,crosshair.Green,crosshair.Blue,crosshair.Opacity};
            if(ReadColor(crosshairJson,"color",color)){crosshair.Red=color.r;crosshair.Green=color.g;crosshair.Blue=color.b;crosshair.Opacity=color.a;}
        }
        Crosshair::SetConfig(crosshair); // Missing section: disabled defaults, not stale previous state.

        WorldEnvironment::Config environment{};
        const auto environmentJson=ExtractSection(json,"world_environment");
        if(!environmentJson.empty())
        {
            ReadBool(environmentJson,"night_enabled",environment.NightEnabled);
            ReadFloat(environmentJson,"night_strength",environment.Strength);
            ReadFloat(environmentJson,"coolness",environment.Coolness);
        }
        WorldEnvironment::SetConfig(environment); // Old profiles safely default to disabled.

        // --- MapHack ---
        std::string mapJson = ExtractSection(json, "maphack");
        if (!mapJson.empty())
        {
            ReadBool(mapJson, "enabled",    MapHack::cfg.Enabled);
            ReadBool(mapJson, "enemy_gods", MapHack::cfg.ShowEnemyGods);
            ReadBool(mapJson, "wards",      MapHack::cfg.ShowEnemyWards);
            ReadBool(mapJson, "familiars",  MapHack::cfg.ShowEnemyFamiliars);
            ReadBool(mapJson, "jungle",     MapHack::cfg.ShowJungle);
        }

        std::string camJson = ExtractSection(json, "camera");
        if (!camJson.empty())
        {
            ReadBool (camJson, "enable_fov",       Camera::cfg.EnableFov);
            ReadFloat(camJson, "fov_value",         Camera::cfg.FovValue);
            ReadBool (camJson, "enable_distance",  Camera::cfg.EnableDistance);
            ReadFloat(camJson, "distance_value",   Camera::cfg.DistanceValue);
            ReadBool (camJson, "no_collision",     Camera::cfg.NoArmCollision);
        }

        std::string menuJson = ExtractSection(json, "menu");
        if (!menuJson.empty())
        {
            DeathCamera::Config death{};
            ReadBool(menuJson,"death_camera_enabled",death.Enabled);
            DeathCamera::SetConfig(death);
            auto hud=StatusOverlay::GetConfig();
            ReadBool(menuJson,"show_fps",hud.ShowFps);
            ReadBool(menuJson,"show_ping",hud.ShowPing);
            ReadBool(menuJson,"show_spectators",hud.ShowSpectators);
            StatusOverlay::SetConfig(hud);
            AutoLaugh::Config laugh{};
            ReadBool(menuJson,"auto_laugh_enabled",laugh.Enabled);
            ReadInt(menuJson,"auto_laugh_key",laugh.HoldKey);
            ReadFloat(menuJson,"auto_laugh_interval",laugh.IntervalSeconds);
            AutoLaugh::SetConfig(laugh);
            auto autoPick = AutoPick::GetConfig();
            ReadBool(menuJson, "auto_pick_enabled", autoPick.Enabled);
            ReadInt(menuJson, "auto_pick_mode", autoPick.Mode);
            ReadFloat(menuJson, "auto_pick_seconds", autoPick.TriggerSeconds);
            ReadString(menuJson, "auto_pick_god", autoPick.GodKey);
            AutoPick::SetConfig(autoPick);
            ReadBool(menuJson, "auto_dagger", AutoDagger::cfg.Enabled);
            ReadInt(menuJson, "auto_dagger_retry_ms", AutoDagger::cfg.RetryIntervalMs);
            AutoDagger::Reset();
            int designVersion = 0;
            ReadInt(menuJson, "menu_design_version", designVersion);
            if (designVersion >= 5)
            {
                ReadInt(menuJson, "theme_preset", UITheme::appearance.Preset);
                ReadColor(menuJson, "theme_accent", UITheme::appearance.Accent);
                ReadColor(menuJson, "theme_background", UITheme::appearance.Background);
                ReadColor(menuJson, "theme_surface", UITheme::appearance.Surface);
                ReadColor(menuJson, "theme_sidebar", UITheme::appearance.Sidebar);
                ReadColor(menuJson, "theme_text", UITheme::appearance.Text);
                ReadFloat(menuJson, "theme_opacity", UITheme::appearance.Opacity);
                ReadFloat(menuJson, "theme_rounding", UITheme::appearance.Rounding);
                ReadFloat(menuJson, "theme_border", UITheme::appearance.BorderStrength);
            }
            else
            {
                // First load of the new design resets appearance only, not game settings.
                UITheme::appearance = UITheme::AppearanceSettings{};
            }
            // Older configurations have no comfort settings: use readable defaults.
            UITheme::comfort = UITheme::ComfortSettings{};
            ReadFloat(menuJson, "menu_scale", UITheme::comfort.Scale);
            ReadBool(menuJson, "menu_tooltips", UITheme::comfort.Tooltips);
            UITheme::SanitizeComfort();
            ReadBool (menuJson, "particles",         UITheme::menuFx.ParticlesEnabled);
            ReadInt  (menuJson, "particle_mode",     UITheme::menuFx.ParticleMode);
            ReadFloat(menuJson, "particle_density",  UITheme::menuFx.ParticleDensity);
            ReadFloat(menuJson, "particle_speed",    UITheme::menuFx.ParticleSpeed);
        }
        return true;
    }

    void ResetInMemory(ResetTarget target)
    {
        const bool all=target==ResetTarget::All;
        if(all||target==ResetTarget::Visuals) ::ESP::cfg = ::ESP::Config{};

        // Silent Aim Experiment Toggle (new experimental feature)
        if(all || target == ResetTarget::Combat)
        {
            silentAimExperimentEnabled = false;
            silentAimExperimentType = 0;
        }
        if(all||target==ResetTarget::Combat){::Aimbot::cfg = ::Aimbot::Config{};Aimbot::ResetState();}
        if(all||target==ResetTarget::Map) ::MapHack::cfg = ::MapHack::Config{};
        if(all||target==ResetTarget::Environment){
            WorldEnvironment::SetConfig(WorldEnvironment::Config{});
            TimeDilationExperiment::SetConfig(TimeDilationExperiment::Config{});
        }
        if(all||target==ResetTarget::Camera) ::Camera::cfg = ::Camera::Config{};
        if(all||target==ResetTarget::Cooldowns){
            ::CooldownWindow::cfg = ::CooldownWindow::Config{};
            CooldownWindow::ResetWidgetLayoutCache();
        }
        if(all||target==ResetTarget::Overlay){
            // Uses the module's existing disable/restore path; never discards camera ownership.
            DeathCamera::SetConfig(DeathCamera::Config{});
            StatusOverlay::SetConfig(StatusOverlay::Config{});
            Crosshair::SetConfig(Crosshair::Config{});
        }
        if(all||target==ResetTarget::Automation){
            AutoLaugh::SetConfig(AutoLaugh::Config{});
            AutoPick::SetConfig(AutoPick::Config{});
            AutoDagger::cfg = AutoDagger::Config{};
            AutoDagger::Reset();
        }
        if(all){
            UITheme::menuFx = UITheme::MenuFxSettings{};
            UITheme::appearance = UITheme::AppearanceSettings{};
            UITheme::comfort = UITheme::ComfortSettings{};
        }
    }
    void SaveDefault()
    {
        ResetInMemory(ResetTarget::All);
        Save();
    }
}
