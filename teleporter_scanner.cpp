#include "teleporter_scanner.h"
#include "teleporter.h"
#include "../core/sdk_helpers.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <string>
#include <unordered_set>

namespace TeleporterScanner
{
    static char s_logPath[MAX_PATH + 32] = {};
    static char s_lastLine[512] = {};
    static FILE* s_logFile = nullptr;

    static SDK::UHWInteractableComponent* s_prevCurrentInteractable = nullptr;
    static std::unordered_set<uintptr_t> s_prevActiveAbilityPtrs;
    static std::unordered_set<std::string> s_prevEffectNames;
    static std::unordered_set<uintptr_t> s_prevInFieldInteractables;
    static float s_tickTimer = 0.f;

    struct SehActiveAbilityHit
    {
        uintptr_t ptr = 0;
        char name[160]{};
        char className[160]{};
    };

    struct SehEffectHit
    {
        char name[160]{};
    };

    static bool SafeRead(uintptr_t addr, void* out, size_t size)
    {
        __try {
            if (!Helpers::IsValidPtr(reinterpret_cast<const void*>(addr)))
                return false;
            memcpy(out, reinterpret_cast<const void*>(addr), size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool FNameToUtf8Seh(const SDK::FName& fname, char* out, size_t outLen)
    {
        out[0] = '\0';
        __try {
            if (fname.IsNone())
                return false;

            wchar_t wbuf[160] = {};
            UC::FString fstr(wbuf, 0, 160);
            if (!SDK::FName::AppendString)
                SDK::FName::InitInternal();

            auto appendFn = reinterpret_cast<void(*)(const SDK::FName*, UC::FString&)>(
                SDK::FName::AppendString);
            SDK::InSDKUtils::CallGameFunction(appendFn, &fname, fstr);

            const wchar_t* w = fstr.CStr();
            if (!w || !*w)
                return false;
            return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen, nullptr, nullptr) > 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CopyObjectNameSeh(SDK::UObject* obj, char* out, size_t outLen)
    {
        out[0] = '\0';
        if (!obj)
            return false;
        __try {
            if (!Helpers::IsValidPtr(obj))
                return false;
            return FNameToUtf8Seh(obj->Name, out, outLen);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool NameHintsPortalKeyword(const char* upperName)
    {
        if (!upperName || !*upperName)
            return false;
        return strstr(upperName, "PORTAL") != nullptr ||
               strstr(upperName, "TELEPORT") != nullptr ||
               strstr(upperName, "GATE") != nullptr ||
               strstr(upperName, "INTERACT") != nullptr;
    }

    static void ToUpperAscii(char* s)
    {
        if (!s)
            return;
        for (; *s; ++s)
            *s = (char)std::toupper((unsigned char)*s);
    }

    static void EnsureLogPath()
    {
        if (s_logPath[0])
            return;

        char temp[MAX_PATH + 1] = {};
        GetTempPathA(MAX_PATH, temp);
        snprintf(s_logPath, sizeof(s_logPath), "%shw_teleporter_scan.log", temp);
    }

    static void WriteLogLine(const char* line)
    {
        if (!line)
            return;

        EnsureLogPath();

        if (!s_logFile)
            fopen_s(&s_logFile, s_logPath, "a");

        if (s_logFile)
        {
            time_t now = time(nullptr);
            struct tm tmLocal{};
            localtime_s(&tmLocal, &now);
            char ts[32]{};
            strftime(ts, sizeof(ts), "%H:%M:%S", &tmLocal);
            fprintf(s_logFile, "[%s] %s\n", ts, line);
            fflush(s_logFile);
        }

        strncpy_s(s_lastLine, sizeof(s_lastLine), line, _TRUNCATE);
        OutputDebugStringA("[tp-scan] ");
        OutputDebugStringA(line);
        OutputDebugStringA("\n");
    }

    static void Logf(const char* fmt, ...)
    {
        char line[1024]{};
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        WriteLogLine(line);
    }

    static SDK::UHWAbilitySystemComponent* GetAscSeh(SDK::AHWCharacter_Base* ch)
    {
        __try {
            return ch ? ch->GetHWAbilitySystemComponent() : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static SDK::UHWInteractableComponent* GetInteractFromActorSeh(SDK::AActor* actor)
    {
        if (!actor || !Helpers::IsValidPtr(actor))
            return nullptr;
        __try {
            auto* comp = actor->GetComponentByClass(SDK::UHWInteractableComponent::StaticClass());
            if (!comp || !Helpers::IsValidPtr(comp))
                return nullptr;
            return static_cast<SDK::UHWInteractableComponent*>(comp);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static void LogTagSeh(const char* prefix, const SDK::FGameplayTag& tag)
    {
        char tagName[128]{};
        FNameToUtf8Seh(tag.TagName, tagName, sizeof(tagName));
        Logf("%s tag=%s", prefix, tagName[0] ? tagName : "(none)");
    }

    static void LogTagContainerSeh(const char* prefix, const SDK::FGameplayTagContainer& container)
    {
        char joined[1024]{};
        joined[0] = '\0';

        __try {
            const int n = container.GameplayTags.Num();
            if (n <= 0)
            {
                Logf("%s (empty)", prefix);
                return;
            }

            size_t used = 0;
            for (int i = 0; i < n && i < 24; ++i)
            {
                if (!container.GameplayTags.IsValidIndex(i))
                    continue;

                char tagName[128]{};
                if (!FNameToUtf8Seh(container.GameplayTags[i].TagName, tagName, sizeof(tagName)))
                    continue;

                if (used > 0 && used + 2 < sizeof(joined))
                {
                    joined[used++] = ',';
                    joined[used++] = ' ';
                    joined[used] = '\0';
                }

                strncat_s(joined, sizeof(joined), tagName, _TRUNCATE);
                used = strlen(joined);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("%s (read failed)", prefix);
            return;
        }

        Logf("%s [%s]", prefix, joined[0] ? joined : "(empty)");
    }

    static bool SehReadInteractField(SDK::UHWInteractableComponent* interact,
                                     SDK::AHWCharacter_Base* ch,
                                     bool* outInField,
                                     bool* outCanUse,
                                     float* outDist)
    {
        if (outInField) *outInField = false;
        if (outCanUse) *outCanUse = false;
        if (outDist) *outDist = -1.f;
        if (!interact || !ch)
            return false;

        __try {
            SDK::AActor* owner = interact->GetOwner();
            if (outInField) *outInField = interact->IsInInteractionField(ch);
            if (outCanUse) *outCanUse = interact->CanInteractWith(ch);
            if (outDist && owner)
            {
                *outDist = Helpers::Dist3D(Helpers::GetLocation(ch), Helpers::GetLocation(owner));
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool SehReadTowerPortalFields(SDK::ABP_Interactable_TowerPortals_C* portal,
                                         bool* canTeleport,
                                         int32_t* owningTeam,
                                         bool* lanePusher,
                                         double* cooldown,
                                         SDK::AActor** linkedTarget,
                                         SDK::FGameplayTag* teleportTag)
    {
        __try {
            if (canTeleport) *canTeleport = portal->CanTeleport;
            if (owningTeam) *owningTeam = portal->OwningTeam;
            if (lanePusher) *lanePusher = portal->bTeleportToLanePusher;
            if (cooldown) *cooldown = portal->DefaultCooldownDuration;
            if (linkedTarget) *linkedTarget = portal->TargetTeleportActor;
            if (teleportTag) *teleportTag = portal->TeleportTag;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void DumpInteractable(SDK::UHWInteractableComponent* interact,
                                 SDK::AHWCharacter_Base* ch,
                                 const char* reason)
    {
        if (!interact)
            return;

        SDK::AActor* owner = nullptr;
        __try { owner = interact->GetOwner(); } __except (EXCEPTION_EXECUTE_HANDLER) {}

        char ownerName[160]{};
        char className[160]{};
        SDK::FVector loc{};
        if (owner)
        {
            CopyObjectNameSeh(owner, ownerName, sizeof(ownerName));
            if (owner->Class)
                CopyObjectNameSeh(owner->Class, className, sizeof(className));
            loc = Helpers::GetLocation(owner);
        }

        Logf("--- %s ---", reason ? reason : "interactable");
        Logf("  component=%p owner=%p", interact, owner);
        if (owner)
        {
            Logf("  ownerName=%s", ownerName[0] ? ownerName : "?");
            Logf("  ownerClass=%s", className[0] ? className : "?");
            Logf("  ownerLoc=(%.0f, %.0f, %.0f)", loc.X, loc.Y, loc.Z);
        }

        bool inField = false;
        bool canUse = false;
        float dist = -1.f;
        if (SehReadInteractField(interact, ch, &inField, &canUse, &dist))
        {
            Logf("  inField=%s canInteract=%s dist=%.0f",
                 inField ? "yes" : "no", canUse ? "yes" : "no", dist);
        }

        if (owner && owner->IsA(SDK::ABP_Interactable_TowerPortals_C::StaticClass()))
        {
            auto* portal = static_cast<SDK::ABP_Interactable_TowerPortals_C*>(owner);
            bool canTeleport = false;
            int32_t owningTeam = 0;
            bool lanePusher = false;
            double cooldown = 0.0;
            SDK::AActor* linkedTarget = nullptr;
            SDK::FGameplayTag teleportTag{};
            if (SehReadTowerPortalFields(portal, &canTeleport, &owningTeam, &lanePusher,
                                         &cooldown, &linkedTarget, &teleportTag))
            {
                Logf("  TowerPortals: CanTeleport=%s OwningTeam=%d lanePusher=%s cooldown=%.1f linkedTarget=%p",
                     canTeleport ? "yes" : "no", owningTeam,
                     lanePusher ? "yes" : "no", cooldown, linkedTarget);
                LogTagSeh("  TowerPortals TeleportTag", teleportTag);
            }
        }
        else if (className[0] && strstr(className, "PortalGate_Joust") != nullptr)
        {
            Logf("  JoustPortalGate: use BaseTeleporter chain, not GA_COM_Interact");
        }
    }

    static SDK::UHWAbility_Interact* FindInteractAbilityInstanceSeh(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        if (!hwAsc)
            return nullptr;

        __try {
            const auto& items = hwAsc->ActivatableAbilities.Items;
            const int n = items.Num();
            for (int i = 0; i < n; ++i)
            {
                if (!items.IsValidIndex(i))
                    continue;

                SDK::UGameplayAbility* ability = items[i].Ability;
                if (!ability || !Helpers::IsValidPtr(ability))
                    continue;
                if (!ability->IsA(SDK::UHWAbility_Interact::StaticClass()))
                    continue;

                const auto& instances = items[i].NonReplicatedInstances;
                for (int j = 0; j < instances.Num(); ++j)
                {
                    if (!instances.IsValidIndex(j))
                        continue;
                    SDK::UGameplayAbility* inst = instances[j];
                    if (inst && Helpers::IsValidPtr(inst))
                        return static_cast<SDK::UHWAbility_Interact*>(inst);
                }

                const auto& replicated = items[i].ReplicatedInstances;
                for (int j = 0; j < replicated.Num(); ++j)
                {
                    if (!replicated.IsValidIndex(j))
                        continue;
                    SDK::UGameplayAbility* inst = replicated[j];
                    if (inst && Helpers::IsValidPtr(inst))
                        return static_cast<SDK::UHWAbility_Interact*>(inst);
                }

                return static_cast<SDK::UHWAbility_Interact*>(ability);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return nullptr;
    }

    static bool SehReadAbilityInstanceActive(SDK::UGameplayAbility* inst, bool* outActive,
                                             char* outName, size_t outNameLen,
                                             char* outClassName, size_t outClassLen)
    {
        if (outActive) *outActive = false;
        if (outName && outNameLen) outName[0] = '\0';
        if (outClassName && outClassLen) outClassName[0] = '\0';
        if (!inst || !Helpers::IsValidPtr(inst))
            return false;

        __try {
            if (outActive) *outActive = inst->bIsActive;
            if (outName && outNameLen)
                CopyObjectNameSeh(inst, outName, outNameLen);
            if (outClassName && outClassLen && inst->Class)
                CopyObjectNameSeh(inst->Class, outClassName, outClassLen);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void DumpAbilitySpecSeh(SDK::FGameplayAbilitySpec* spec)
    {
        if (!spec)
            return;

        SDK::UGameplayAbility* ability = nullptr;
        uint8_t activeCount = 0;
        uint8_t inputPressed = 0;
        SDK::FGameplayTagContainer dynamicTags{};

        __try {
            ability = spec->Ability;
            activeCount = spec->ActiveCount;
            inputPressed = spec->InputPressed;
            dynamicTags = spec->DynamicAbilityTags;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        if (!ability || !Helpers::IsValidPtr(ability))
            return;

        char abilityName[160]{};
        CopyObjectNameSeh(ability, abilityName, sizeof(abilityName));
        char upper[160]{};
        strncpy_s(upper, sizeof(upper), abilityName, _TRUNCATE);
        ToUpperAscii(upper);

        if (!NameHintsPortalKeyword(upper) && !strstr(upper, "INTERACT") && !strstr(upper, "TELEPORTER") &&
            activeCount == 0)
            return;

        if (strstr(upper, "GROUNDTARGETHELPER") != nullptr)
            return;

        Logf("  ability %s activeCount=%u inputPressed=%u",
             abilityName, (unsigned)activeCount, (unsigned)inputPressed);
        LogTagContainerSeh("    dynamicTags", dynamicTags);

        SDK::UGameplayAbility* instances[8]{};
        int instanceCount = 0;

        __try {
            for (int j = 0; j < spec->NonReplicatedInstances.Num() && instanceCount < 8; ++j)
            {
                if (!spec->NonReplicatedInstances.IsValidIndex(j))
                    continue;
                instances[instanceCount++] = spec->NonReplicatedInstances[j];
            }
            for (int j = 0; j < spec->ReplicatedInstances.Num() && instanceCount < 8; ++j)
            {
                if (!spec->ReplicatedInstances.IsValidIndex(j))
                    continue;
                instances[instanceCount++] = spec->ReplicatedInstances[j];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        for (int i = 0; i < instanceCount; ++i)
        {
            bool active = false;
            char instName[160]{};
            if (!SehReadAbilityInstanceActive(instances[i], &active, instName, sizeof(instName), nullptr, 0))
                continue;
            Logf("    instance %s bIsActive=%s ptr=%p",
                 instName[0] ? instName : "?", active ? "yes" : "no", instances[i]);
        }
    }

    static bool AbilityClassNameInteresting(const char* className)
    {
        if (!className || !*className)
            return false;

        char upper[160]{};
        strncpy_s(upper, sizeof(upper), className, _TRUNCATE);
        ToUpperAscii(upper);

        if (strstr(upper, "GROUNDTARGETHELPER") != nullptr)
            return false;

        return NameHintsPortalKeyword(upper) ||
               strstr(upper, "INTERACT") != nullptr ||
               strstr(upper, "TELEPORTER") != nullptr;
    }

    static void TrackActiveAbilityInstance(SDK::UGameplayAbility* inst,
                                           SehActiveAbilityHit* outHits,
                                           int maxHits,
                                           int* inOutCount)
    {
        if (!inst || !outHits || !inOutCount || *inOutCount >= maxHits)
            return;

        bool active = false;
        if (!SehReadAbilityInstanceActive(inst, &active, outHits[*inOutCount].name,
                                          sizeof(outHits[*inOutCount].name),
                                          outHits[*inOutCount].className,
                                          sizeof(outHits[*inOutCount].className)))
            return;
        if (!active)
            return;
        if (!AbilityClassNameInteresting(outHits[*inOutCount].className))
            return;

        outHits[*inOutCount].ptr = reinterpret_cast<uintptr_t>(inst);
        ++(*inOutCount);
    }

    static int SehCollectNewActiveAbilities(SDK::UHWAbilitySystemComponent* hwAsc,
                                            SehActiveAbilityHit* outHits,
                                            int maxHits)
    {
        if (!hwAsc || !outHits || maxHits <= 0)
            return 0;

        int count = 0;
        __try {
            const auto& items = hwAsc->ActivatableAbilities.Items;
            for (int i = 0; i < items.Num() && count < maxHits; ++i)
            {
                if (!items.IsValidIndex(i))
                    continue;

                const SDK::FGameplayAbilitySpec& spec = items[i];
                for (int j = 0; j < spec.NonReplicatedInstances.Num(); ++j)
                {
                    if (spec.NonReplicatedInstances.IsValidIndex(j))
                        TrackActiveAbilityInstance(spec.NonReplicatedInstances[j], outHits, maxHits, &count);
                }
                for (int j = 0; j < spec.ReplicatedInstances.Num(); ++j)
                {
                    if (spec.ReplicatedInstances.IsValidIndex(j))
                        TrackActiveAbilityInstance(spec.ReplicatedInstances[j], outHits, maxHits, &count);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return count;
    }

    static void ScanActiveAbilities(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        SehActiveAbilityHit hits[16]{};
        const int hitCount = SehCollectNewActiveAbilities(hwAsc, hits, 16);

        std::unordered_set<uintptr_t> current;
        for (int i = 0; i < hitCount; ++i)
        {
            current.insert(hits[i].ptr);
            if (s_prevActiveAbilityPtrs.find(hits[i].ptr) == s_prevActiveAbilityPtrs.end())
            {
                Logf(">>> NEW ACTIVE ABILITY: %s (%p)", hits[i].name, (void*)hits[i].ptr);
                Logf("    class=%s", hits[i].className[0] ? hits[i].className : "?");
            }
        }

        s_prevActiveAbilityPtrs = std::move(current);
    }

    static bool EffectNameInteresting(const char* name)
    {
        if (!name || !*name)
            return false;
        char upper[160]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        ToUpperAscii(upper);
        return NameHintsPortalKeyword(upper) ||
               strstr(upper, "TRAVEL") != nullptr ||
               strstr(upper, "MAPTELEPORT") != nullptr;
    }

    static int SehCollectInterestingEffects(SDK::UHWAbilitySystemComponent* hwAsc,
                                            SehEffectHit* outHits,
                                            int maxHits)
    {
        if (!hwAsc || !outHits || maxHits <= 0)
            return 0;

        int count = 0;
        __try {
            const auto& effects = hwAsc->ActiveGameplayEffects.GameplayEffects_Internal;
            for (int i = 0; i < effects.Num() && count < maxHits; ++i)
            {
                if (!effects.IsValidIndex(i))
                    continue;

                const SDK::UGameplayEffect* def = effects[i].spec.Def;
                if (!def || !Helpers::IsValidPtr(def))
                    continue;

                if (!CopyObjectNameSeh(const_cast<SDK::UGameplayEffect*>(def),
                                       outHits[count].name, sizeof(outHits[count].name)))
                    continue;

                if (!EffectNameInteresting(outHits[count].name))
                    continue;

                ++count;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return count;
    }

    static void SehDumpAllAbilitySpecs(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        if (!hwAsc)
            return;
        __try {
            const auto& items = hwAsc->ActivatableAbilities.Items;
            for (int i = 0; i < items.Num(); ++i)
            {
                if (!items.IsValidIndex(i))
                    continue;
                DumpAbilitySpecSeh(const_cast<SDK::FGameplayAbilitySpec*>(&items[i]));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void SehLogEffectGrantedTags(SDK::UHWAbilitySystemComponent* hwAsc, const char* effectName)
    {
        if (!hwAsc || !effectName || !*effectName)
            return;

        __try {
            const auto& effects = hwAsc->ActiveGameplayEffects.GameplayEffects_Internal;
            for (int j = 0; j < effects.Num(); ++j)
            {
                if (!effects.IsValidIndex(j))
                    continue;
                const SDK::UGameplayEffect* def = effects[j].spec.Def;
                if (!def)
                    continue;
                char name[160]{};
                if (!CopyObjectNameSeh(const_cast<SDK::UGameplayEffect*>(def), name, sizeof(name)))
                    continue;
                if (strcmp(name, effectName) != 0)
                    continue;
                LogTagContainerSeh("    grantedTags", def->InheritableGameplayEffectTags.CombinedTags);
                break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void ScanActiveEffects(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        SehEffectHit hits[32]{};
        const int hitCount = SehCollectInterestingEffects(hwAsc, hits, 32);

        std::unordered_set<std::string> current;
        for (int i = 0; i < hitCount; ++i)
        {
            current.insert(hits[i].name);
            if (s_prevEffectNames.find(hits[i].name) == s_prevEffectNames.end())
            {
                Logf(">>> NEW ACTIVE EFFECT: %s", hits[i].name);
                SehLogEffectGrantedTags(hwAsc, hits[i].name);
            }
        }

        s_prevEffectNames = std::move(current);
    }

    static bool SehIsInInteractionField(SDK::UHWInteractableComponent* interact,
                                        SDK::AHWCharacter_Base* ch)
    {
        __try {
            return interact && ch && interact->IsInInteractionField(ch);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void ScanInFieldInteractables(SDK::AHWCharacter_Base* ch)
    {
        std::unordered_set<uintptr_t> current;
        for (SDK::AActor* actor : Helpers::GetAllLevelActors())
        {
            SDK::UHWInteractableComponent* interact = GetInteractFromActorSeh(actor);
            if (!interact)
                continue;
            if (!SehIsInInteractionField(interact, ch))
                continue;

            const uintptr_t key = reinterpret_cast<uintptr_t>(interact);
            current.insert(key);
            if (s_prevInFieldInteractables.find(key) == s_prevInFieldInteractables.end())
                DumpInteractable(interact, ch, "entered interaction field");
        }

        s_prevInFieldInteractables = std::move(current);
    }

    static SDK::UHWInteractableComponent* SehReadCurrentInteractable(SDK::UHWAbility_Interact* ability)
    {
        if (!ability)
            return nullptr;
        __try { return ability->CurrentInteractable; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static SDK::UHWInteractableComponent* SehFindClosestInteractable(SDK::UHWAbility_Interact* ability,
                                                                    SDK::AHWCharacter_Base* ch)
    {
        if (!ability || !ch)
            return nullptr;
        __try { return ability->FindClosestInteractable(ch); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static void MonitorCurrentInteractable(SDK::AHWCharacter_Base* ch, SDK::UHWAbilitySystemComponent* hwAsc)
    {
        SDK::UHWAbility_Interact* interactAbility = FindInteractAbilityInstanceSeh(hwAsc);
        SDK::UHWInteractableComponent* current = SehReadCurrentInteractable(interactAbility);

        if (current == s_prevCurrentInteractable)
            return;

        if (current)
        {
            Logf(">>> CurrentInteractable changed -> %p", current);
            DumpInteractable(current, ch, "CurrentInteractable");

            if (interactAbility)
            {
                SDK::UHWInteractableComponent* closest = SehFindClosestInteractable(interactAbility, ch);
                if (closest)
                {
                    Logf("  FindClosestInteractable -> %p", closest);
                    if (closest != current)
                        DumpInteractable(closest, ch, "FindClosestInteractable");
                }
            }
        }
        else if (s_prevCurrentInteractable)
        {
            Logf(">>> CurrentInteractable cleared (was %p)", s_prevCurrentInteractable);
        }

        s_prevCurrentInteractable = current;
    }

    static void LogMatchContext()
    {
        char gameStateName[160]{};
        char lane[64]{};
        bool joust = false;

        __try {
            SDK::UWorld* world = Helpers::GetWorld();
            if (!world || !world->GameState)
                return;

            CopyObjectNameSeh(world->GameState, gameStateName, sizeof(gameStateName));

            if (world->GameState->IsA(SDK::ABP_JoustGameState_C::StaticClass()))
            {
                joust = true;
                auto* gs = static_cast<SDK::ABP_JoustGameState_C*>(world->GameState);
                FNameToUtf8Seh(gs->JoustLane, lane, sizeof(lane));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        Logf("gameState=%s", gameStateName[0] ? gameStateName : "?");
        if (joust)
            Logf("JoustLane=%s", lane[0] ? lane : "(none)");
    }

    static bool SehActorIsPortalCandidate(SDK::AActor* actor, bool* outTower, bool* outInteractable,
                                          bool* outHasInteract, bool* outNameHint)
    {
        if (outTower) *outTower = false;
        if (outInteractable) *outInteractable = false;
        if (outHasInteract) *outHasInteract = false;
        if (outNameHint) *outNameHint = false;
        if (!actor)
            return false;

        __try {
            if (outTower)
                *outTower = actor->IsA(SDK::ABP_Interactable_TowerPortals_C::StaticClass());
            if (outInteractable)
                *outInteractable = actor->IsA(SDK::ABP_Interactable_C::StaticClass());
            if (outHasInteract)
                *outHasInteract = GetInteractFromActorSeh(actor) != nullptr;

            char actorName[160]{};
            char className[160]{};
            CopyObjectNameSeh(actor, actorName, sizeof(actorName));
            if (actor->Class)
                CopyObjectNameSeh(actor->Class, className, sizeof(className));
            char upper[160]{};
            strncpy_s(upper, sizeof(upper), actorName, _TRUNCATE);
            ToUpperAscii(upper);
            if (outNameHint)
            {
                *outNameHint = NameHintsPortalKeyword(upper) ||
                               NameHintsPortalKeyword(className);
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void Reset()
    {
        if (s_logFile)
        {
            fclose(s_logFile);
            s_logFile = nullptr;
        }

        s_prevCurrentInteractable = nullptr;
        s_prevActiveAbilityPtrs.clear();
        s_prevEffectNames.clear();
        s_prevInFieldInteractables.clear();
        s_tickTimer = 0.f;
        s_lastLine[0] = '\0';
    }

    void ClearLog()
    {
        EnsureLogPath();
        if (s_logFile)
        {
            fclose(s_logFile);
            s_logFile = nullptr;
        }
        DeleteFileA(s_logPath);
        s_lastLine[0] = '\0';
        WriteLogLine("log cleared");
    }

    const char* GetLogPath()
    {
        EnsureLogPath();
        return s_logPath;
    }

    const char* GetLastLine()
    {
        return s_lastLine;
    }

    void ScanWorld()
    {
        EnsureLogPath();
        Logf("======== WORLD SCAN ========");
        LogMatchContext();

        SDK::AHWCharacter_Base* ch = Helpers::GetLocalChar();
        SDK::UHWAbilitySystemComponent* hwAsc = GetAscSeh(ch);
        if (!ch)
        {
            Logf("no local character");
            return;
        }

        Logf("player=%s team=%u", Helpers::GetPlayerName(ch).c_str(), (unsigned)Helpers::GetTeamId(ch));

        int interactableCount = 0;
        int towerPortalCount = 0;
        int portalLikeCount = 0;

        for (SDK::AActor* actor : Helpers::GetAllLevelActors())
        {
            if (!actor || !Helpers::IsValidPtr(actor))
                continue;

            bool isTower = false;
            bool isInteractable = false;
            bool hasInteract = false;
            bool nameHint = false;
            if (!SehActorIsPortalCandidate(actor, &isTower, &isInteractable, &hasInteract, &nameHint))
                continue;

            if (!isTower && !isInteractable && !hasInteract && !nameHint)
                continue;

            char actorName[160]{};
            char className[160]{};
            CopyObjectNameSeh(actor, actorName, sizeof(actorName));
            if (actor->Class)
                CopyObjectNameSeh(actor->Class, className, sizeof(className));

            ++interactableCount;
            if (isTower)
                ++towerPortalCount;
            if (nameHint || isTower)
                ++portalLikeCount;

            const SDK::FVector loc = Helpers::GetLocation(actor);
            Logf("[%d] actor=%s class=%s dist=%.0f loc=(%.0f,%.0f,%.0f)",
                 interactableCount,
                 actorName[0] ? actorName : "?",
                 className[0] ? className : "?",
                 Helpers::Dist3D(Helpers::GetLocation(ch), loc),
                 loc.X, loc.Y, loc.Z);

            if (hasInteract)
                DumpInteractable(GetInteractFromActorSeh(actor), ch, "scan");
        }

        Logf("summary: interactables=%d towerPortals=%d portalLike=%d",
             interactableCount, towerPortalCount, portalLikeCount);

        if (hwAsc)
        {
            Logf("--- active abilities (teleport/interact/portal) ---");
            SehDumpAllAbilitySpecs(hwAsc);
            ScanActiveEffects(hwAsc);
        }

        SDK::UHWAbility_Interact* interactAbility = FindInteractAbilityInstanceSeh(hwAsc);
        SDK::UHWInteractableComponent* current = SehReadCurrentInteractable(interactAbility);
        if (current)
            DumpInteractable(current, ch, "CurrentInteractable@scan");

        Logf("======== END SCAN ========");
        Logf("Use the pad manually with scanner enabled to capture live ability/effect events.");
    }

    void Tick()
    {
        return;

        s_tickTimer += 0.016f;
        if (s_tickTimer < 0.10f)
            return;
        s_tickTimer = 0.f;

        SDK::AHWCharacter_Base* ch = Helpers::GetLocalChar();
        SDK::UHWAbilitySystemComponent* hwAsc = GetAscSeh(ch);
        if (!ch || !hwAsc)
            return;

        MonitorCurrentInteractable(ch, hwAsc);
        ScanInFieldInteractables(ch);
        ScanActiveAbilities(hwAsc);
        ScanActiveEffects(hwAsc);
    }
}
