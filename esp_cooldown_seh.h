#pragma once
#include "../core/smite_sdk.h"
#include <cstdint>

#define ESP_CD_MAX_RELIC_SLOTS 1

namespace EspCooldownSeh
{
    enum class EspAbilitySlotState : uint8_t
    {
        Locked = 0,
        Ready,
        OnCooldown
    };

    struct EspAbilitySlotInfo
    {
        int   slot      = 0;
        EspAbilitySlotState state = EspAbilitySlotState::Locked;
        float remaining = 0.f;
        float duration  = 0.f;
    };

    static constexpr int kMaxCooldowns = 4;
    static constexpr int kMaxRelicSlots = ESP_CD_MAX_RELIC_SLOTS;

    bool GetEnemyAbilityStates(SDK::APawn* pawn, float worldTime,
                               EspAbilitySlotInfo out[kMaxCooldowns]);
    bool GetEnemyRelicStates(SDK::APawn* pawn, float worldTime,
                             EspAbilitySlotInfo out[kMaxRelicSlots],
                             char relicCodeOut[48] = nullptr);
}
