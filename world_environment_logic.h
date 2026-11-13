#pragma once
#include <cmath>
namespace WorldEnvironment { namespace Logic {
    inline float Unit(float value,float fallback)
    { return !std::isfinite(value)?fallback:(value<0.f?0.f:(value>1.f?1.f:value)); }
    struct Gain { double X=1.0,Y=1.0,Z=1.0,W=1.0; };
    inline bool Finite(const Gain& g)
    { return std::isfinite(g.X)&&std::isfinite(g.Y)&&std::isfinite(g.Z)&&std::isfinite(g.W); }
    inline bool Same(const Gain& a,const Gain& b)
    { return a.X==b.X&&a.Y==b.Y&&a.Z==b.Z&&a.W==b.W; }
    inline Gain Night(const Gain& base,float strength,float coolness)
    {
        const double s=static_cast<double>(Unit(strength,0.65f)),c=static_cast<double>(Unit(coolness,0.60f));
        const double dim=1.0-0.60*s;
        // Only attenuate: no night-vision / shadow boosting. Preserve global W.
        return {base.X*dim*(1.0-0.18*c*s),base.Y*dim*(1.0-0.07*c*s),base.Z*dim,base.W};
    }
}}
