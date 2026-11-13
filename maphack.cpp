#include "maphack.h"
#include "../core/sdk_helpers.h"
#include "../core/game_state.h"
#include <unordered_map>

namespace MapHack
{
    Config cfg;

    struct RevealedEntry
    {
        SDK::UHWVisibilityComponent* Vis = nullptr;
        bool WasAlwaysVisible = false;
    };

    static std::unordered_map<uintptr_t, RevealedEntry> s_revealed;
    static bool s_wasEnabled = false;

    static bool IsEnemyToLocal(SDK::APawn* localPawn, SDK::AHWCharacter_Base* ch)
    {
        if (!localPawn || !ch)
            return false;
        return Helpers::IsHostileToLocal(localPawn, ch);
    }

    static void TrackAndReveal(SDK::UHWVisibilityComponent* visComp)
    {
        if (!visComp || !Helpers::IsValidPtr(visComp))
            return;

        const uintptr_t key = reinterpret_cast<uintptr_t>(visComp);
        auto it = s_revealed.find(key);
        if (it == s_revealed.end())
        {
            RevealedEntry entry{};
            entry.Vis = visComp;
            entry.WasAlwaysVisible = visComp->bAlwaysVisible;
            s_revealed.emplace(key, entry);
        }

        visComp->bAlwaysVisible = true;
    }

    static void EnableMinimapIcon(SDK::AHWCharacter_Base* ch)
    {
        __try
        {
            if (!ch || !Helpers::IsValidPtr(ch))
                return;

            auto* abilityActor = ch->GetCharacterAbilitySystemActor();
            if (!abilityActor || !Helpers::IsValidPtr(abilityActor))
                return;

            SDK::UHWVisibilityComponent* visComp = abilityActor->VisibilityComponent;
            if (!visComp || !Helpers::IsValidPtr(visComp))
                return;

            TrackAndReveal(visComp);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void ProcessCharacter(SDK::AHWCharacter_Base* ch,
                                 SDK::APawn* localPawn)
    {
        if (!ch || !localPawn || ch == localPawn)
            return;
        if (Helpers::IsDead(ch))
            return;
        if (!IsEnemyToLocal(localPawn, ch))
            return;

        bool shouldReveal = false;

        if (cfg.ShowEnemyGods && ch->IsAGod())
            shouldReveal = true;

        if (cfg.ShowEnemyWards)
        {
            const bool isGod       = ch->IsAGod();
            const bool isMinion    = ch->IsAMinion();
            const bool isJungle    = ch->IsAJungleMonster() || ch->IsAJungleBoss();
            const bool isStructure = ch->IsAStructure() || ch->IsATitan();
            const bool isFamiliar  = ch->IsFamiliar();
            if (!isGod && !isMinion && !isJungle && !isStructure && !isFamiliar)
                shouldReveal = true;
        }

        if (cfg.ShowEnemyFamiliars && ch->IsFamiliar())
            shouldReveal = true;

        if (cfg.ShowJungle && (ch->IsAJungleMonster() || ch->IsAJungleBoss()))
            shouldReveal = true;

        if (shouldReveal)
            EnableMinimapIcon(ch);
    }

    static void PruneStaleRevealed()
    {
        for (auto it = s_revealed.begin(); it != s_revealed.end();)
        {
            if (!it->second.Vis || !Helpers::IsValidPtr(it->second.Vis))
                it = s_revealed.erase(it);
            else
                ++it;
        }
    }

    void Reset()
    {
        for (auto& pair : s_revealed)
        {
            RevealedEntry& entry = pair.second;
            __try
            {
                if (!entry.Vis || !Helpers::IsValidPtr(entry.Vis))
                    continue;

                entry.Vis->bAlwaysVisible = entry.WasAlwaysVisible;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        s_revealed.clear();
    }

    static void ProcessPawnSafe(SDK::APawn* pawn, SDK::APawn* localPawn)
    {
        __try {
            if (!pawn || !Helpers::IsValidPtr(pawn))
                return;
            if (!pawn->IsA(SDK::AHWCharacter_Base::StaticClass()))
                return;

            ProcessCharacter(static_cast<SDK::AHWCharacter_Base*>(pawn), localPawn);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static SDK::APawn* SafeGetLocalPawn()
    {
        SDK::APawn* localPawn = nullptr;
        __try {
            auto* localPc = Helpers::GetLocalPC();
            localPawn = localPc ? localPc->K2_GetPawn() : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return localPawn;
    }

    void Tick()
    {
        if (s_wasEnabled && !cfg.Enabled)
            Reset();
        s_wasEnabled = cfg.Enabled;

        if (!cfg.Enabled)
            return;

        if (!GameState::IsPlayable())
            return;

        PruneStaleRevealed();

        SDK::APawn* localPawn = SafeGetLocalPawn();
        const auto pawns = Helpers::GetAllPawns();
        for (SDK::APawn* pawn : pawns)
            ProcessPawnSafe(pawn, localPawn);
    }
}
