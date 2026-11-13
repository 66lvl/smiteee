#pragma once
#include "crosshair_logic.h"
#include "../imgui/imgui.h"
#include "../ui/theme.h"
#include <mutex>

// Header-only by design: no project-file changes and no SDK/game-memory access.
namespace Crosshair
{
    inline std::mutex configMutex;
    inline Config config{};
    inline Config GetConfig(){std::lock_guard<std::mutex> lock(configMutex);return config;}
    inline void SetConfig(Config value){std::lock_guard<std::mutex> lock(configMutex);config=Sanitize(value);}
    inline void Render(ImDrawList* dl,ImVec2 center,const Config& raw,float scale)
    {
        if(!dl)return;
        const Config c=Sanitize(raw);const auto g=Build(c,scale);
        const ImU32 color=ImGui::ColorConvertFloat4ToU32({c.Red,c.Green,c.Blue,c.Opacity});
        const ImU32 border=ImGui::ColorConvertFloat4ToU32({0.f,0.f,0.f,c.Opacity*c.OutlineOpacity});
        const float o=c.OutlineThickness*scale;
        if(c.Outline&&o>0.f)
        {
            for(int i=0;i<g.Count;++i){const auto r=g.Rects[i];dl->AddRectFilled({center.x+r.X0-o,center.y+r.Y0-o},{center.x+r.X1+o,center.y+r.Y1+o},border);}
            if(g.Ring)dl->AddCircle(center,g.RingRadius,border,64,g.RingThickness+2.f*o);
        }
        for(int i=0;i<g.Count;++i){const auto r=g.Rects[i];dl->AddRectFilled({center.x+r.X0,center.y+r.Y0},{center.x+r.X1,center.y+r.Y1},color);}
        if(g.Ring)dl->AddCircle(center,g.RingRadius,color,64,g.RingThickness);
    }
    inline void Draw(bool menuOpen,bool focused,bool gameContext)
    {
        const Config c=GetConfig();if(!ShouldDraw(c,menuOpen,focused,gameContext))return;
        auto* viewport=ImGui::GetMainViewport();if(!viewport||viewport->Size.x<=0.f||viewport->Size.y<=0.f)return;
        const ImVec2 center{viewport->Pos.x+viewport->Size.x*0.5f,viewport->Pos.y+viewport->Size.y*0.5f};
        auto* dl=ImGui::GetBackgroundDrawList();
        dl->PushClipRect(viewport->Pos,{viewport->Pos.x+viewport->Size.x,viewport->Pos.y+viewport->Size.y},true);
        Render(dl,center,c,ScreenScale(c,viewport->Size.y));dl->PopClipRect();
    }
    inline void Preview(const Config& c)
    {
        static float zoom=1.f;
        UITheme::InfoText("LIVE PREVIEW - visible here even while the overlay is disabled");
        ImGui::SliderFloat("Preview zoom",&zoom,1.f,4.f,"%.1fx");
        ImGui::BeginChild("crosshair_live_preview",{0.f,UITheme::Px(210.f)},true,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 p=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();
        const ImVec2 center{p.x+size.x*0.5f,p.y+size.y*0.5f};auto* dl=ImGui::GetWindowDrawList();
        dl->AddRectFilled(p,{p.x+size.x,p.y+size.y},IM_COL32(22,30,43,255));
        dl->AddLine({p.x,center.y},{p.x+size.x,center.y},IM_COL32(50,60,75,150));
        dl->AddLine({center.x,p.y},{center.x,p.y+size.y},IM_COL32(50,60,75,150));
        Render(dl,center,c,ScreenScale(c,ImGui::GetMainViewport()->Size.y)*zoom);
        ImGui::Dummy(size);ImGui::EndChild();
        UITheme::InfoText("Geometric screen center, not a predicted impact point. Does not remove the game's own reticle. Dimensions do not follow menu text scale.");
    }
    inline void DrawSettings()
    {
        Config c=GetConfig();bool changed=false;
        UITheme::BeginCard("custom_crosshair",UITheme::Icon::Target,"Custom crosshair","Static screen-center reticle; no aiming or weapon changes.");
        changed|=UITheme::CheckboxRow("Enable custom crosshair",&c.Enabled);
        UITheme::HelpLastItem("Enable custom crosshair","Draws a separate static reticle at the center of the game window. Does not change aim, spread, recoil or the game's original HUD.");
        if(ImGui::BeginTable("crosshair_editor_columns",ImGui::GetContentRegionAvail().x>=UITheme::Px(650.f)?2:1,ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            int preset=0;const char* presets[]={"Choose a preset...","Classic green","Compact cyan","White dot","Ring + dot"};
            if(UITheme::ComboRow("Apply look preset",&preset,presets,5)&&preset>0){c=Preset(preset-1,c);changed=true;}
            const char* styles[]={"Cross","T shape","Dot","Circle"};
            changed|=UITheme::ComboRow("Shape",&c.Style,styles,4);
            float rgb[]={c.Red,c.Green,c.Blue};
            if(ImGui::ColorEdit3("Crosshair color",rgb)){c.Red=rgb[0];c.Green=rgb[1];c.Blue=rgb[2];changed=true;}
            changed|=UITheme::SliderFloatRow("Opacity",&c.Opacity,0.f,1.f,"%.2f");
            ImGui::BeginDisabled(c.Style>=2);
            changed|=UITheme::SliderFloatRow("Line length",&c.Length,0.f,40.f,"%.1f px");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(c.Style==2);
            changed|=UITheme::SliderFloatRow("Line thickness",&c.Thickness,1.f,8.f,"%.1f px");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(c.Style>=2);
            changed|=UITheme::SliderFloatRow("Center gap",&c.Gap,0.f,30.f,"%.1f px");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(c.Style==2);
            changed|=UITheme::CheckboxRow("Center dot",&c.CenterDot);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(c.Style!=2&&!c.CenterDot);
            changed|=UITheme::SliderFloatRow("Dot size",&c.DotSize,1.f,10.f,"%.1f px");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(c.Style!=3);
            changed|=UITheme::SliderFloatRow("Circle radius",&c.Radius,2.f,40.f,"%.1f px");
            ImGui::EndDisabled();
            changed|=UITheme::CheckboxRow("Black outline",&c.Outline);
            ImGui::BeginDisabled(!c.Outline);
            changed|=UITheme::SliderFloatRow("Outline thickness",&c.OutlineThickness,0.f,4.f,"%.1f px");
            changed|=UITheme::SliderFloatRow("Outline opacity",&c.OutlineOpacity,0.f,1.f,"%.2f");
            ImGui::EndDisabled();
            ImGui::TableNextColumn();Preview(c);
            changed|=UITheme::CheckboxRow("Hide while menu is open",&c.HideInMenu);
            changed|=UITheme::CheckboxRow("Require loaded game context",&c.RequireGameContext);
            UITheme::HelpLastItem("Require loaded game context","Uses the project's cached GameState::IsPlayable check. It is not an exact in-match/ADS detector; a lobby with a local controller may also pass it.");
            changed|=UITheme::CheckboxRow("Scale with screen height",&c.ScaleWithResolution);
            UITheme::HelpLastItem("Scale with screen height","Off: fixed ImGui screen-coordinate dimensions. On: 1080-high reference size. No dependence on menu scale. High-DPI backends may map ImGui units to multiple physical pixels.");
            if(c.Opacity<=0.f||(!c.CenterDot&&c.Style<2&&c.Length<=0.f))UITheme::InfoText("No visible reticle with the current opacity/geometry.");
            if(ImGui::Button("Reset crosshair..."))ImGui::OpenPopup("Confirm crosshair reset");
            if(ImGui::BeginPopupModal("Confirm crosshair reset",nullptr,ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+fminf(UITheme::Px(360.f),fmaxf(100.f,ImGui::GetIO().DisplaySize.x-60.f)));
                ImGui::TextWrapped("Reset only the crosshair to defaults (disabled)? Other settings and the saved profile stay unchanged.");
                ImGui::PopTextWrapPos();
                if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();
                ImGui::SameLine();if(ImGui::Button("Reset crosshair")){c=Config{};changed=true;ImGui::CloseCurrentPopup();}
                ImGui::EndPopup();
            }
            ImGui::EndTable();
        }
        if(changed)SetConfig(c);
        UITheme::InfoText("Static presets inspired by tactical-shooter reticles, not CS2/Valorant share-code import or a spread/recoil simulation. Save changes explicitly to keep this profile.");
        UITheme::EndCard();
    }
}
