#include "auto_dagger.h"
#include "auto_dagger_logic.h"
#include "../core/sdk_helpers.h"
#include <Windows.h>
#include <cstdio>
#include <string>

namespace AutoDagger
{
    Config cfg;
    static Logic::Gate s_gate;
    static SDK::AHWCharacter_Base* s_character = nullptr;
    static SDK::UWorld* s_world = nullptr;
    static ULONGLONG s_nextScan = 0;
    static const char* s_status = "Disabled";

    void Reset()
    {
        cfg.RetryIntervalMs = Logic::ClampRetryInterval(cfg.RetryIntervalMs);
        s_gate = Logic::Gate{};
        s_character = nullptr;
        s_world = nullptr;
        s_nextScan = 0;
        s_status = cfg.Enabled ? "Waiting for a character" : "Disabled";
    }
    const char* Status() { return s_status; }

    static bool GameHasFocus()
    {
        DWORD pid = 0;
        HWND window = GetForegroundWindow();
        if (!window) return false;
        GetWindowThreadProcessId(window, &pid);
        return pid == GetCurrentProcessId();
    }

    // Kept separate from SEH functions: std::string needs C++ stack unwinding.
    static bool ItemMatches(SDK::UHWEquipmentItem* item)
    {
        return item && Logic::IsDaggerIdentifier(item->GetName());
    }

    static bool CheckAndRequest(SDK::AHWCharacter_Base* character, ULONGLONG now)
    {
        __try
        {
            auto* inventory = character->GetInventoryManager();
            auto* asc = character->GetHWAbilitySystemComponent();
            if (!Helpers::IsValidPtr(inventory) || !Helpers::IsValidPtr(asc))
            {
                s_status = "Inventory or ability system unavailable";
                return false;
            }
            const auto& entries = inventory->Inventory.Entries;
            const int count = entries.Num();
            if (count < 0 || count > 64)
            {
                s_status = "Inventory data invalid; no activation";
                return false;
            }
            s_status = "Obsidian Dagger not identified";
            for (int i = 0; i < count; ++i)
            {
                if (!entries.IsValidIndex(i)) continue;
                auto* instance = entries[i].EquipmentInstance;
                if (!Helpers::IsValidPtr(instance) || !instance->bIsEquipped) continue;
                auto* item = instance->EquipmentItem;
                if (!Helpers::IsValidPtr(item) || !ItemMatches(item)) continue;
                const SDK::FGameplayAbilitySpecHandle handle = instance->GrantedAbilityHandle;
                if (handle.Handle <= 0 || asc->GetEquipmentInstanceFromAbility(handle) != instance)
                {
                    s_status = "Dagger found; active ability not verified";
                    return false;
                }
                // Verify the handle against this character's current ability list.
                bool specFound = false;
                bool isActive = false;
                const auto& specs = asc->ActivatableAbilities.Items;
                const int specCount = specs.Num();
                if (specCount < 0 || specCount > 512)
                {
                    s_status = "Ability data invalid; no activation";
                    return false;
                }
                for (int j = 0; j < specCount; ++j)
                {
                    if (!specs.IsValidIndex(j)) continue;
                    if (specs[j].Handle.Handle == handle.Handle)
                    {
                        specFound = !specs[j].PendingRemove && specs[j].Level > 0;
                        isActive = specs[j].ActiveCount != 0;
                        break;
                    }
                }
                if (!specFound) { s_status = "Dagger ability unavailable"; return false; }
                if (isActive) { s_status = "Dagger ability already active"; return false; }
                if (!asc->CanActivateAbility(handle, nullptr))
                {
                    s_status = "Dagger on cooldown or activation blocked";
                    return false;
                }
                if (!s_gate.CanAttempt(now)) { s_status = "Waiting before retry"; return false; }
                // Recheck health immediately before requesting activation.
                const double hp = static_cast<double>(character->GetHealth());
                if (!std::isfinite(hp) || hp <= 0.0 || hp > Logic::HealthThreshold || character->IsDead())
                {
                    s_status = "Health condition no longer met";
                    return false;
                }
                // Reserve retry time even if the SDK call faults.
                s_gate.RecordAttempt(now, cfg.RetryIntervalMs);
                const bool accepted = asc->TryActivateAbility(handle, true);
                s_status = accepted ? "Activation requested; retries remain enabled" : "Activation rejected; will retry";
                return accepted;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s_status = "SDK access failed; no further action this tick";
        }
        return false;
    }

    static void TickCharacter(ULONGLONG now)
    {
        __try
        {
            auto* world = Helpers::GetWorld();
            auto* character = Helpers::GetLocalChar();
            if (!Helpers::IsValidPtr(world) || !Helpers::IsValidPtr(character))
            {
                Reset();
                return;
            }
            if (world != s_world || character != s_character)
            {
                s_gate = Logic::Gate{};
                s_world = world;
                s_character = character;
            }
            const double hp = static_cast<double>(character->GetHealth());
            if (character->IsDead() || !std::isfinite(hp) || hp <= 0.0)
            {
                s_gate = Logic::Gate{};
                s_status = "Character dead or health unavailable";
                return;
            }
            if (!s_gate.ObserveHealth(hp))
            {
                s_status = "Armed; HP above 499";
                return;
            }
            CheckAndRequest(character, now);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s_status = "SDK access failed; waiting";
        }
    }

    void Tick(bool playable, bool menuOpen)
    {
        cfg.RetryIntervalMs = Logic::ClampRetryInterval(cfg.RetryIntervalMs);
        if (!cfg.Enabled || !playable) { Reset(); return; }
        if (menuOpen) return; // Preserve the last check so it can be read in Settings.
        if (!GameHasFocus()) { s_status = "Paused while game is unfocused"; return; }
        const ULONGLONG now = GetTickCount64();
        if (now < s_nextScan) return;
        s_nextScan = now + 50; // One scan at most per tick; no catch-up/burst loop.
        TickCharacter(now);
    }

    static void PrintInventoryImpl(SDK::AHWCharacter_Base* character)
    {
        auto* inventory = character->GetInventoryManager();
        if (!Helpers::IsValidPtr(inventory)) return;
        const auto& entries = inventory->Inventory.Entries;
        const int count = entries.Num();
        if (count < 0 || count > 64) return;
        std::printf("[AutoDagger] Inventory item identifiers (%d entries):\n", count);
        for (int i = 0; i < count; ++i)
        {
            if (!entries.IsValidIndex(i)) continue;
            auto* instance = entries[i].EquipmentInstance;
            if (!Helpers::IsValidPtr(instance) || !Helpers::IsValidPtr(instance->EquipmentItem)) continue;
            const std::string name = instance->EquipmentItem->GetName();
            std::printf("[AutoDagger] slot=%d equipped=%d handle=%d name=%s\n",
                entries[i].SlotId.ID, instance->bIsEquipped ? 1 : 0,
                instance->GrantedAbilityHandle.Handle, name.c_str());
        }
    }
    void PrintInventory()
    {
        __try
        {
            auto* character = Helpers::GetLocalChar();
            if (Helpers::IsValidPtr(character)) PrintInventoryImpl(character);
            else std::printf("[AutoDagger] No local character.\n");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            std::printf("[AutoDagger] Inventory read failed.\n");
        }
    }
}
