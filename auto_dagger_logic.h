#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace AutoDagger::Logic
{
    inline constexpr double HealthThreshold = 499.0;
    inline constexpr int MinRetryIntervalMs = 50;
    inline constexpr int MaxRetryIntervalMs = 2000;
    inline constexpr int DefaultRetryIntervalMs = 100;
    inline int ClampRetryInterval(int value)
    {
        if (value < MinRetryIntervalMs) return MinRetryIntervalMs;
        if (value > MaxRetryIntervalMs) return MaxRetryIntervalMs;
        return value;
    }

    inline bool IsDaggerIdentifier(const std::string& input)
    {
        std::string normalized;
        normalized.reserve(input.size());
        for (unsigned char ch : input)
        {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch + ('a' - 'A'));
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
                normalized.push_back(static_cast<char>(ch));
        }
        return normalized.find("obsidiandagger") != std::string::npos;
    }

    struct Gate
    {
        std::uint64_t nextAttemptMs = 0;
        bool ObserveHealth(double health) const
        {
            return std::isfinite(health) && health > 0.0 && health <= HealthThreshold;
        }
        bool CanAttempt(std::uint64_t now) const { return now >= nextAttemptMs; }
        void RecordAttempt(std::uint64_t now, int intervalMs)
        {
            nextAttemptMs = now + static_cast<std::uint64_t>(ClampRetryInterval(intervalMs));
        }
    };
}
