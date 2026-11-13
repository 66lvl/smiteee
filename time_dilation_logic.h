#pragma once
#include <cmath>
namespace TimeDilationExperiment::Logic
{
    inline float Multiplier(float value)
    {
        if (!std::isfinite(value)) return 1.f;
        return value < 1.f ? 1.f : (value > 1.5f ? 1.5f : value);
    }
    inline bool Desired(float baseline, float multiplier, float minimum, float maximum, float* result)
    {
        if (!result || !std::isfinite(baseline) || baseline <= 0.f ||
            !std::isfinite(minimum) || !std::isfinite(maximum) || minimum <= 0.f || maximum < minimum)
            return false;
        const float value = baseline * Multiplier(multiplier);
        if (!std::isfinite(value)) return false;
        *result = value < minimum ? minimum : (value > maximum ? maximum : value);
        return true;
    }
    inline bool StillOwned(float current, float written)
    { return std::isfinite(current) && current == written; }
}
