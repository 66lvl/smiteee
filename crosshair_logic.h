#pragma once
#include <cmath>

namespace Crosshair
{
    enum class Shape : int { Cross=0, T=1, Dot=2, Circle=3 };
    struct Config
    {
        bool Enabled=false,HideInMenu=true,RequireGameContext=true,ScaleWithResolution=false;
        int Style=0;
        float Length=6.f,Thickness=2.f,Gap=4.f,DotSize=2.f,Radius=9.f;
        bool CenterDot=false,Outline=true;
        float OutlineThickness=1.f,Opacity=1.f,OutlineOpacity=0.85f;
        float Red=0.25f,Green=1.f,Blue=0.45f;
    };
    inline float Bounded(float value,float low,float high,float fallback)
    {return !std::isfinite(value)?fallback:(value<low?low:(value>high?high:value));}
    inline Config Sanitize(Config c)
    {
        if(c.Style<0||c.Style>3)c.Style=0;
        c.Length=Bounded(c.Length,0.f,40.f,6.f);c.Thickness=Bounded(c.Thickness,1.f,8.f,2.f);
        c.Gap=Bounded(c.Gap,0.f,30.f,4.f);c.DotSize=Bounded(c.DotSize,1.f,10.f,2.f);c.Radius=Bounded(c.Radius,2.f,40.f,9.f);
        c.OutlineThickness=Bounded(c.OutlineThickness,0.f,4.f,1.f);
        c.Opacity=Bounded(c.Opacity,0.f,1.f,1.f);c.OutlineOpacity=Bounded(c.OutlineOpacity,0.f,1.f,0.85f);
        c.Red=Bounded(c.Red,0.f,1.f,0.25f);c.Green=Bounded(c.Green,0.f,1.f,1.f);c.Blue=Bounded(c.Blue,0.f,1.f,0.45f);
        return c;
    }
    struct Rect {float X0,Y0,X1,Y1;};
    struct Geometry {Rect Rects[5]{};int Count=0;bool Ring=false;float RingRadius=0.f,RingThickness=0.f;};
    inline float ScreenScale(const Config& c,float viewportHeight)
    {return c.ScaleWithResolution?Bounded(viewportHeight/1080.f,0.25f,4.f,1.f):1.f;}
    inline Geometry Build(Config raw,float rawScale=1.f)
    {
        const auto c=Sanitize(raw);const float s=Bounded(rawScale,0.1f,12.f,1.f);
        Geometry g{};const float h=c.Thickness*s*0.5f,l=c.Length*s,d=c.Gap*s;
        const auto style=static_cast<Shape>(c.Style);
        if(style==Shape::Cross||style==Shape::T)
        {
            if(l>0.f)
            {
                g.Rects[g.Count++]={-d-l,-h,-d,h};g.Rects[g.Count++]={d,-h,d+l,h};
                g.Rects[g.Count++]={-h,d,h,d+l};
                if(style==Shape::Cross)g.Rects[g.Count++]={-h,-d-l,h,-d};
            }
        }
        else if(style==Shape::Circle){g.Ring=true;g.RingRadius=c.Radius*s;g.RingThickness=c.Thickness*s;}
        if(c.CenterDot||style==Shape::Dot){const float half=c.DotSize*s*0.5f;g.Rects[g.Count++]={-half,-half,half,half};}
        return g;
    }
    inline bool ShouldDraw(const Config& c,bool menuOpen,bool focused,bool gameContext)
    {return c.Enabled&&c.Opacity>0.f&&focused&&(!c.HideInMenu||!menuOpen)&&(!c.RequireGameContext||gameContext);}
    inline Config Preset(int index,const Config& current)
    {
        Config next{};
        if(index==1){next.Red=0.f;next.Green=0.9f;next.Blue=1.f;next.Length=4.f;next.Gap=3.f;next.Thickness=1.f;}
        else if(index==2){next.Style=2;next.Red=1.f;next.Green=1.f;next.Blue=1.f;next.DotSize=3.f;}
        else if(index==3){next.Style=3;next.Red=1.f;next.Green=0.85f;next.Blue=0.25f;next.Radius=8.f;next.Thickness=1.5f;next.CenterDot=true;}
        next.Enabled=current.Enabled;next.HideInMenu=current.HideInMenu;next.RequireGameContext=current.RequireGameContext;next.ScaleWithResolution=current.ScaleWithResolution;
        return next;
    }
}
