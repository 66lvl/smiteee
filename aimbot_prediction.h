#pragma once
#include <cmath>
#include <cstdint>

// Small platform-independent helpers; positions are cm and velocities cm/s.
namespace AimPrediction
{
    struct Vec3 { double x = 0.0, y = 0.0, z = 0.0; };

    inline bool Finite(const Vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
    inline double Dot(const Vec3& a, const Vec3& b)
    {
        return a.x*b.x + a.y*b.y + a.z*b.z;
    }

    // Constant-velocity straight-line intercept. Returns false when there is no
    // positive solution or when extrapolation is too far into the future.
    inline bool InterceptTime(const Vec3& pos, const Vec3& vel, double speed, double& time)
    {
        time = 0.0;
        if (!Finite(pos) || !Finite(vel) || !std::isfinite(speed) || speed <= 1.0)
            return false;
        const double c = Dot(pos, pos);
        if (c < 1e-12) return true;
        const double vv = Dot(vel, vel);
        const double a = vv - speed*speed;
        const double b = 2.0 * Dot(pos, vel);
        double best = -1.0;
        if (std::abs(a) <= 1e-10 * (vv + speed*speed + 1.0))
        {
            if (std::abs(b) > 1e-12) best = -c / b;
        }
        else
        {
            const double disc = b*b - 4.0*a*c;
            if (disc < 0.0 || !std::isfinite(disc)) return false;
            const double q = -0.5 * (b + std::copysign(std::sqrt(disc), b));
            if (std::abs(q) > 1e-12)
            {
                const double t0 = q / a, t1 = c / q;
                if (t0 > 0.0) best = t0;
                if (t1 > 0.0 && (best < 0.0 || t1 < best)) best = t1;
            }
        }
        if (!std::isfinite(best) || best <= 0.0 || best > 3.0) return false;
        time = best;
        return true;
    }

    struct VelocityFilter
    {
        const void* target = nullptr;
        Vec3 value{};
        std::uint64_t lastMs = 0;
        bool valid = false;

        void Reset() { target = nullptr; value = {}; lastMs = 0; valid = false; }

        Vec3 Update(const void* newTarget, Vec3 sample, std::uint64_t nowMs)
        {
            if (!newTarget || !Finite(sample)) { Reset(); return {}; }
            if (!valid || newTarget != target || nowMs <= lastMs || nowMs - lastMs > 250)
            {
                target = newTarget; value = sample; lastMs = nowMs; valid = true;
                return value;
            }
            const double dt = (nowMs - lastMs) * 0.001;
            lastMs = nowMs;
            const double oldHorizontal = value.x*value.x + value.y*value.y;
            const double newHorizontal = sample.x*sample.x + sample.y*sample.y;
            const double dotHorizontal = value.x*sample.x + value.y*sample.y;
            // Do not average opposite directions: this makes reversals lag badly.
            if (oldHorizontal > 100.0 && newHorizontal > 100.0 && dotHorizontal <= 0.0)
            {
                value = sample;
                return value;
            }
            // Fast decay on stopping; otherwise smooth small velocity fluctuations.
            const double tau = newHorizontal < oldHorizontal * 0.25 ? 0.025 : 0.06;
            const double alpha = 1.0 - std::exp(-dt / tau);
            value.x += alpha * (sample.x - value.x);
            value.y += alpha * (sample.y - value.y);
            value.z += alpha * (sample.z - value.z);
            return value;
        }
    };
}
