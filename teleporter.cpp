#include "teleporter.h"
#include "../core/sdk_helpers.h"
#include "../ui/theme.h"
#include "../imgui/imgui.h"
#include <Windows.h>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

namespace Teleporter
{
    Config cfg;

    enum class PortalKind : uint8_t
    {
        Tower,
        JoustGate,
        Aladdin,
        Generic,
    };

    struct PortalPick
    {
        PortalKind kind = PortalKind::Tower;
        SDK::AActor* actor = nullptr;
        SDK::ABP_Interactable_TowerPortals_C* towerPortal = nullptr;
        SDK::ABP_Interactable_C* joustPortal = nullptr;
        SDK::ABP_Aladdin_ArenaManagerSingleton_C* aladdinPortal = nullptr;
        SDK::ABP_Interactable_C* genericPortal = nullptr;
        SDK::UHWInteractableComponent* interact = nullptr;
        bool inField = false;
        bool canInteract = false;
        bool canTeleport = false;
        bool linkedMapPortal = false;
        bool lanePusherPad = false;
        double defaultCooldown = 0.0;
        float distToPlayer = FLT_MAX;
    };

    static SDK::FName StringToFName(const char* s)
    {
        if (!s || !*s)
            return SDK::FName{};
        const size_t len = strlen(s);
        std::wstring ws(len, L'\0');
        for (size_t i = 0; i < len; ++i)
            ws[i] = (wchar_t)(unsigned char)s[i];
        return SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(ws.c_str()));
    }

    static SDK::UHWAbilitySystemComponent* GetAsc(SDK::AHWCharacter_Base* ch)
    {
        __try {
            return ch ? ch->GetHWAbilitySystemComponent() : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static bool RequestTag(const char* name, SDK::FGameplayTag& outTag)
    {
        outTag = SDK::FGameplayTag{};
        return SDK::UHWAIBlueprintLibrary::RequestGameplayTagFromName(StringToFName(name), &outTag);
    }

    static bool IsJoustMatchSeh()
    {
        __try {
            SDK::UWorld* world = Helpers::GetWorld();
            if (!world || !world->GameState || !Helpers::IsValidPtr(world->GameState))
                return false;
            return world->GameState->IsA(SDK::ABP_JoustGameState_C::StaticClass());
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool TryActivateInteractAbility(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        if (!hwAsc)
            return false;

        static const char* kTagNames[] = {
            "InputTag.Ability.Interact",
            "InputTag.Interact",
            "Ability.Input.Interact",
            "Input.Interact",
            "HW.Input.Interact",
            nullptr
        };

        SDK::UAbilitySystemComponent* asc = hwAsc;
        for (int i = 0; kTagNames[i]; ++i)
        {
            SDK::FGameplayTag tag{};
            if (!RequestTag(kTagNames[i], tag))
                continue;

            const SDK::TArray<SDK::FGameplayAbilitySpecHandle> handles =
                hwAsc->GetAbilitySpecHandlesWithInputTag(tag);

            const int count = handles.Num();
            for (int h = 0; h < count; ++h)
            {
                if (!handles.IsValidIndex(h))
                    continue;
                if (asc->TryActivateAbility(handles[h], true))
                    return true;
            }
        }

        return asc->TryActivateAbilityByClass(SDK::UHWAbility_Interact::StaticClass(), true);
    }

    static bool DoesLaneExistSeh(SDK::AHWCharacter_Base* ch, SDK::FName laneId)
    {
        if (!ch || laneId.IsNone())
            return false;
        __try {
            return SDK::UHWAIBlueprintLibrary::DoesLaneExist(ch, laneId);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static SDK::AHWWaypoint* FindLaneWaypointSeh(SDK::AHWCharacter_Base* ch, SDK::FName laneId)
    {
        if (!ch || laneId.IsNone())
            return nullptr;
        __try {
            return SDK::UHWAIBlueprintLibrary::FindLaneWaypoint(ch, laneId);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static SDK::AHWWaypoint* FindBestLaneWaypoint(SDK::AHWCharacter_Base* ch)
    {
        if (!ch)
            return nullptr;

        __try {
            SDK::UWorld* world = Helpers::GetWorld();
            if (world && world->GameState &&
                world->GameState->IsA(SDK::ABP_JoustGameState_C::StaticClass()))
            {
                auto* gs = static_cast<SDK::ABP_JoustGameState_C*>(world->GameState);
                if (!gs->JoustLane.IsNone())
                {
                    SDK::AHWWaypoint* wp = FindLaneWaypointSeh(ch, gs->JoustLane);
                    if (wp)
                        return wp;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        static const char* kLaneNames[] = { "MidA", "DuoA", "SoloA", nullptr };
        for (int i = 0; kLaneNames[i]; ++i)
        {
            const SDK::FName laneId = StringToFName(kLaneNames[i]);
            if (!DoesLaneExistSeh(ch, laneId))
                continue;

            SDK::AHWWaypoint* wp = FindLaneWaypointSeh(ch, laneId);
            if (wp)
                return wp;
        }

        return nullptr;
    }

    static SDK::UHWInteractableComponent* GetInteractFromActor(SDK::AActor* actor)
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

    static bool PortalMatchesTeamSeh(SDK::ABP_Interactable_TowerPortals_C* portal, uint8_t myTeam)
    {
        if (!portal)
            return false;
        __try {
            if (portal->OwningTeam < 0)
                return true;
            if (myTeam != 255 && portal->OwningTeam >= 0 && portal->OwningTeam != (int32_t)myTeam)
                return false;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void ReadInteractFieldSeh(SDK::UHWInteractableComponent* interact,
                                     SDK::AHWCharacter_Base* ch,
                                     bool* outInField,
                                     bool* outCanInteract)
    {
        if (outInField) *outInField = false;
        if (outCanInteract) *outCanInteract = false;
        if (!interact || !ch)
            return;
        __try {
            if (outInField) *outInField = interact->IsInInteractionField(ch);
            if (outCanInteract) *outCanInteract = interact->CanInteractWith(ch);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static bool ReadCanTeleportSeh(SDK::ABP_Interactable_TowerPortals_C* portal)
    {
        if (!portal)
            return false;
        __try { return portal->CanTeleport; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ReadTowerTeleportTagValidSeh(SDK::ABP_Interactable_TowerPortals_C* portal)
    {
        if (!portal)
            return false;
        __try { return !portal->TeleportTag.TagName.IsNone(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ReadLinkedMapPortalSeh(SDK::ABP_Interactable_TowerPortals_C* portal)
    {
        if (!portal)
            return false;
        __try {
            SDK::AActor* target = portal->TargetTeleportActor;
            return target && Helpers::IsValidPtr(target);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static double ReadDefaultCooldownSeh(SDK::ABP_Interactable_TowerPortals_C* portal)
    {
        if (!portal)
            return 0.0;
        __try { return portal->DefaultCooldownDuration; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0; }
    }

    static bool ReadLanePusherPadSeh(SDK::ABP_Interactable_TowerPortals_C* portal)
    {
        if (!portal)
            return false;
        __try { return portal->bTeleportToLanePusher; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CopyClassNameSeh(SDK::UClass* cls, char* out, size_t outLen)
    {
        out[0] = '\0';
        if (!cls)
            return false;
        __try {
            if (!Helpers::IsValidPtr(cls))
                return false;
            wchar_t wbuf[160] = {};
            UC::FString fstr(wbuf, 0, 160);
            if (!SDK::FName::AppendString)
                SDK::FName::InitInternal();
            auto appendFn = reinterpret_cast<void(*)(const SDK::FName*, UC::FString&)>(
                SDK::FName::AppendString);
            SDK::InSDKUtils::CallGameFunction(appendFn, &cls->Name, fstr);
            const wchar_t* w = fstr.CStr();
            if (!w || !*w)
                return false;
            return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen, nullptr, nullptr) > 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ClassNameContainsSeh(SDK::UClass* cls, const char* needle)
    {
        if (!cls || !needle || !*needle)
            return false;
        char name[160]{};
        if (!CopyClassNameSeh(cls, name, sizeof(name)))
            return false;
        char upper[160]{};
        strncpy_s(upper, sizeof(upper), name, _TRUNCATE);
        _strupr_s(upper);
        char needleUpper[160]{};
        strncpy_s(needleUpper, sizeof(needleUpper), needle, _TRUNCATE);
        _strupr_s(needleUpper);
        return strstr(upper, needleUpper) != nullptr;
    }

    static bool IsJoustPortalGateActor(SDK::AActor* actor)
    {
        if (!actor || !Helpers::IsValidPtr(actor))
            return false;
        return ClassNameContainsSeh(actor->Class, "PortalGate_Joust");
    }

    static bool NameHintsMapPortal(const std::string& upperName)
    {
        if (upperName.empty())
            return false;
        return upperName.find("PORTAL") != std::string::npos ||
               upperName.find("TELEPORT") != std::string::npos ||
               upperName.find("GATE") != std::string::npos;
    }

    static bool IsGenericPortalActor(SDK::AActor* actor)
    {
        if (!actor || !Helpers::IsValidPtr(actor))
            return false;
        if (IsJoustPortalGateActor(actor))
            return false;
        if (actor->IsA(SDK::ABP_Interactable_TowerPortals_C::StaticClass()))
            return false;
        if (actor->IsA(SDK::ABP_Aladdin_ArenaManagerSingleton_C::StaticClass()))
            return false;
        if (actor->IsA(SDK::ABP_BattleStandard_Pickup_C::StaticClass()))
            return false;
        if (actor->IsA(SDK::ABP_Interactable_TargetDummy_C::StaticClass()))
            return false;
        if (!actor->IsA(SDK::ABP_Interactable_C::StaticClass()))
            return false;
        if (!GetInteractFromActor(actor))
            return false;

        const std::string name = Helpers::SafeGetActorName(actor);
        return NameHintsMapPortal(name);
    }

    static std::vector<SDK::ABP_Interactable_TowerPortals_C*> CollectTowerPortals(uint8_t myTeam,
                                                                                    bool joustMatch)
    {
        std::vector<SDK::ABP_Interactable_TowerPortals_C*> portals;
        portals.reserve(8);

        for (SDK::AActor* actor : Helpers::GetAllLevelActors())
        {
            if (!actor || !Helpers::IsValidPtr(actor))
                continue;
            if (!actor->IsA(SDK::ABP_Interactable_TowerPortals_C::StaticClass()))
                continue;

            auto* portal = static_cast<SDK::ABP_Interactable_TowerPortals_C*>(actor);
            if (!joustMatch && !PortalMatchesTeamSeh(portal, myTeam))
                continue;

            portals.push_back(portal);
        }

        return portals;
    }

    static std::vector<SDK::ABP_Interactable_C*> CollectJoustPortalGates()
    {
        std::vector<SDK::ABP_Interactable_C*> portals;
        portals.reserve(4);

        for (SDK::AActor* actor : Helpers::GetAllLevelActors())
        {
            if (!IsJoustPortalGateActor(actor))
                continue;
            if (!actor->IsA(SDK::ABP_Interactable_C::StaticClass()))
                continue;
            portals.push_back(static_cast<SDK::ABP_Interactable_C*>(actor));
        }

        return portals;
    }

    static std::vector<SDK::ABP_Interactable_C*> CollectGenericPortals()
    {
        std::vector<SDK::ABP_Interactable_C*> portals;
        portals.reserve(4);

        for (SDK::AActor* actor : Helpers::GetAllLevelActors())
        {
            if (!IsGenericPortalActor(actor))
                continue;
            portals.push_back(static_cast<SDK::ABP_Interactable_C*>(actor));
        }

        return portals;
    }

    static std::vector<SDK::ABP_Aladdin_ArenaManagerSingleton_C*> CollectAladdinPortals()
    {
        std::vector<SDK::ABP_Aladdin_ArenaManagerSingleton_C*> portals;
        portals.reserve(2);

        for (SDK::AActor* actor : Helpers::GetAllLevelActors())
        {
            if (!actor || !Helpers::IsValidPtr(actor))
                continue;
            if (!actor->IsA(SDK::ABP_Aladdin_ArenaManagerSingleton_C::StaticClass()))
                continue;

            portals.push_back(static_cast<SDK::ABP_Aladdin_ArenaManagerSingleton_C*>(actor));
        }

        return portals;
    }

    static PortalPick BuildTowerPortalPick(SDK::ABP_Interactable_TowerPortals_C* portal,
                                           SDK::AHWCharacter_Base* ch)
    {
        PortalPick pick{};
        if (!portal || !ch)
            return pick;

        pick.kind = PortalKind::Tower;
        pick.actor = portal;
        pick.towerPortal = portal;
        pick.interact = GetInteractFromActor(portal);
        if (pick.interact)
            ReadInteractFieldSeh(pick.interact, ch, &pick.inField, &pick.canInteract);
        pick.canTeleport = ReadCanTeleportSeh(portal);
        pick.linkedMapPortal = ReadLinkedMapPortalSeh(portal);
        pick.lanePusherPad = ReadLanePusherPadSeh(portal);
        pick.defaultCooldown = ReadDefaultCooldownSeh(portal);
        pick.distToPlayer = Helpers::Dist3D(Helpers::GetLocation(ch), Helpers::GetLocation(portal));
        return pick;
    }

    static PortalPick BuildJoustPortalPick(SDK::ABP_Interactable_C* portal,
                                           SDK::AHWCharacter_Base* ch)
    {
        PortalPick pick{};
        if (!portal || !ch)
            return pick;

        pick.kind = PortalKind::JoustGate;
        pick.actor = portal;
        pick.joustPortal = portal;
        pick.interact = GetInteractFromActor(portal);
        if (pick.interact)
            ReadInteractFieldSeh(pick.interact, ch, &pick.inField, &pick.canInteract);
        pick.distToPlayer = Helpers::Dist3D(Helpers::GetLocation(ch), Helpers::GetLocation(portal));
        return pick;
    }

    static PortalPick BuildGenericPortalPick(SDK::ABP_Interactable_C* portal,
                                             SDK::AHWCharacter_Base* ch)
    {
        PortalPick pick{};
        if (!portal || !ch)
            return pick;

        pick.kind = PortalKind::Generic;
        pick.actor = portal;
        pick.genericPortal = portal;
        pick.interact = GetInteractFromActor(portal);
        if (pick.interact)
            ReadInteractFieldSeh(pick.interact, ch, &pick.inField, &pick.canInteract);
        pick.distToPlayer = Helpers::Dist3D(Helpers::GetLocation(ch), Helpers::GetLocation(portal));
        return pick;
    }

    static PortalPick BuildAladdinPortalPick(SDK::ABP_Aladdin_ArenaManagerSingleton_C* portal,
                                             SDK::AHWCharacter_Base* ch)
    {
        PortalPick pick{};
        if (!portal || !ch)
            return pick;

        pick.kind = PortalKind::Aladdin;
        pick.actor = portal;
        pick.aladdinPortal = portal;
        pick.interact = portal->BP_InteractableComponent;
        if (!pick.interact || !Helpers::IsValidPtr(pick.interact))
            pick.interact = GetInteractFromActor(portal);
        if (pick.interact)
            ReadInteractFieldSeh(pick.interact, ch, &pick.inField, &pick.canInteract);
        pick.canTeleport = true;
        pick.distToPlayer = Helpers::Dist3D(Helpers::GetLocation(ch), Helpers::GetLocation(portal));
        return pick;
    }

    static bool PortalReadyForChain(const PortalPick& pick, bool joustMatch)
    {
        if (!pick.actor || !pick.interact)
            return false;

        if (pick.kind == PortalKind::Tower)
        {
            if (ReadTowerTeleportTagValidSeh(pick.towerPortal))
                return true;
            if (pick.canTeleport)
                return true;
            if (joustMatch)
            {
                if (pick.defaultCooldown > 0.0 && pick.defaultCooldown <= 30.0)
                    return true;
                if (!pick.lanePusherPad)
                    return true;
            }
            if (pick.linkedMapPortal)
                return true;
            return false;
        }

        if (pick.kind == PortalKind::JoustGate)
            return pick.interact != nullptr;

        return true;
    }

    static float ScoreTowerPortal(const PortalPick& pick, bool joustMatch,
                                  const SDK::FVector& chLoc,
                                  const SDK::FVector& wpLoc,
                                  bool hasWaypoint)
    {
        float score = pick.distToPlayer;

        if (pick.inField)
            score *= 0.05f;

        if (ReadTowerTeleportTagValidSeh(pick.towerPortal))
            score *= 0.05f;

        if (joustMatch)
        {
            if (pick.lanePusherPad)
                score *= 8.0f;

            if (pick.defaultCooldown > 60.0)
                score *= 6.0f;
            else if (pick.defaultCooldown > 0.0 && pick.defaultCooldown <= 30.0)
                score *= 0.15f;

            if (pick.linkedMapPortal)
                score *= 0.80f;
        }
        else
        {
            if (pick.linkedMapPortal)
                score *= 0.10f;

            if (hasWaypoint)
            {
                const float wpDist = Helpers::Dist3D(Helpers::GetLocation(pick.towerPortal), wpLoc);
                score = score * 0.35f + wpDist * 0.65f;
            }
        }

        if (pick.canInteract)
            score *= 0.90f;

        return score;
    }

    static std::vector<PortalPick> CollectRankedTowerPortals(SDK::AHWCharacter_Base* ch)
    {
        std::vector<PortalPick> ranked;
        if (!ch)
            return ranked;

        const bool joustMatch = IsJoustMatchSeh();
        const uint8_t myTeam = Helpers::GetTeamId(ch);
        const std::vector<SDK::ABP_Interactable_TowerPortals_C*> portals =
            CollectTowerPortals(myTeam, joustMatch);
        if (portals.empty())
            return ranked;

        SDK::AHWWaypoint* wp = FindBestLaneWaypoint(ch);
        const SDK::FVector wpLoc = wp ? Helpers::GetLocation(wp) : SDK::FVector{};
        const SDK::FVector chLoc = Helpers::GetLocation(ch);
        const bool hasWaypoint = wp != nullptr;

        ranked.reserve(portals.size());
        for (SDK::ABP_Interactable_TowerPortals_C* portal : portals)
        {
            PortalPick pick = BuildTowerPortalPick(portal, ch);
            pick.distToPlayer = Helpers::Dist3D(chLoc, Helpers::GetLocation(portal));
            ranked.push_back(pick);
        }

        std::sort(ranked.begin(), ranked.end(),
                  [&](const PortalPick& a, const PortalPick& b) {
                      return ScoreTowerPortal(a, joustMatch, chLoc, wpLoc, hasWaypoint) <
                             ScoreTowerPortal(b, joustMatch, chLoc, wpLoc, hasWaypoint);
                  });

        return ranked;
    }

    static std::vector<PortalPick> CollectRankedJoustPortals(SDK::AHWCharacter_Base* ch)
    {
        std::vector<PortalPick> ranked;
        if (!ch)
            return ranked;

        const std::vector<SDK::ABP_Interactable_C*> portals = CollectJoustPortalGates();
        if (portals.empty())
            return ranked;

        const SDK::FVector chLoc = Helpers::GetLocation(ch);
        ranked.reserve(portals.size());
        for (SDK::ABP_Interactable_C* portal : portals)
        {
            PortalPick pick = BuildJoustPortalPick(portal, ch);
            ranked.push_back(pick);
        }

        std::sort(ranked.begin(), ranked.end(),
                  [&](const PortalPick& a, const PortalPick& b) {
                      float scoreA = a.distToPlayer;
                      float scoreB = b.distToPlayer;
                      if (a.inField)
                          scoreA *= 0.05f;
                      if (b.inField)
                          scoreB *= 0.05f;
                      if (a.canInteract)
                          scoreA *= 0.90f;
                      if (b.canInteract)
                          scoreB *= 0.90f;
                      return scoreA < scoreB;
                  });

        return ranked;
    }

    static PortalPick PickBestTowerPortal(SDK::AHWCharacter_Base* ch)
    {
        const std::vector<PortalPick> ranked = CollectRankedTowerPortals(ch);
        if (ranked.empty())
            return PortalPick{};

        const bool joustMatch = IsJoustMatchSeh();
        int inFieldCount = 0;
        PortalPick soleInField{};
        for (const PortalPick& pick : ranked)
        {
            if (!pick.inField)
                continue;
            soleInField = pick;
            ++inFieldCount;
        }

        if (inFieldCount == 1)
            return soleInField;

        for (const PortalPick& pick : ranked)
        {
            if (PortalReadyForChain(pick, joustMatch))
                return pick;
        }

        return ranked.front();
    }

    static PortalPick PickBestAladdinPortal(SDK::AHWCharacter_Base* ch)
    {
        PortalPick best{};
        if (!ch)
            return best;

        const std::vector<SDK::ABP_Aladdin_ArenaManagerSingleton_C*> portals = CollectAladdinPortals();
        if (portals.empty())
            return best;

        float bestScore = FLT_MAX;
        for (SDK::ABP_Aladdin_ArenaManagerSingleton_C* portal : portals)
        {
            PortalPick pick = BuildAladdinPortalPick(portal, ch);

            float score = pick.distToPlayer;
            if (pick.inField)
                score *= 0.05f;

            if (score < bestScore)
            {
                bestScore = score;
                best = pick;
            }
        }

        return best;
    }

    static PortalPick PickBestGenericPortal(SDK::AHWCharacter_Base* ch)
    {
        PortalPick best{};
        if (!ch)
            return best;

        const std::vector<SDK::ABP_Interactable_C*> portals = CollectGenericPortals();
        if (portals.empty())
            return best;

        float bestScore = FLT_MAX;
        for (SDK::ABP_Interactable_C* portal : portals)
        {
            PortalPick pick = BuildGenericPortalPick(portal, ch);

            float score = pick.distToPlayer;
            if (pick.inField)
                score *= 0.05f;

            if (score < bestScore)
            {
                bestScore = score;
                best = pick;
            }
        }

        return best;
    }

    static PortalPick PickBestPortal(SDK::AHWCharacter_Base* ch)
    {
        PortalPick pick = PickBestTowerPortal(ch);
        if (pick.actor)
            return pick;

        pick = PickBestAladdinPortal(ch);
        if (pick.actor)
            return pick;

        return PickBestGenericPortal(ch);
    }

    static bool StartBpInteractionSeh(SDK::UHWInteractableComponent* interact,
                                      SDK::AHWCharacter_Base* ch)
    {
        if (!interact || !ch)
            return false;
        __try {
            if (Helpers::SafeIsA(interact, SDK::UBP_InteractableComponent_C::StaticClass()))
            {
                static_cast<SDK::UBP_InteractableComponent_C*>(interact)->StartInteraction(ch);
                return true;
            }
            interact->StartInteraction(ch);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool SetTowerPortalInstigatorSeh(SDK::ABP_Interactable_TowerPortals_C* portal,
                                            SDK::AHWCharacter_Base* ch)
    {
        if (!portal || !ch)
            return false;
        __try {
            portal->InteractInstigator = ch;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool SetAladdinPortalInstigatorSeh(SDK::ABP_Aladdin_ArenaManagerSingleton_C* portal,
                                              SDK::AHWCharacter_Base* ch)
    {
        if (!portal || !ch)
            return false;
        __try {
            portal->Interact_Instigator = ch;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool FireTowerPortalInteractionEventSeh(SDK::ABP_Interactable_TowerPortals_C* portal,
                                                   SDK::UHWInteractableComponent* interact,
                                                   SDK::AHWCharacter_Base* ch)
    {
        if (!portal || !interact || !ch)
            return false;
        __try {
            portal->BndEvt__BP_Interactable_PortalGate_BP_InteractableComponent_K2Node_ComponentBoundEvent_0_OnInteractionEventDel__DelegateSignature(
                interact, ch);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool FireAladdinPortalInteractionEventSeh(SDK::ABP_Aladdin_ArenaManagerSingleton_C* portal,
                                                     SDK::UHWInteractableComponent* interact,
                                                     SDK::AHWCharacter_Base* ch)
    {
        if (!portal || !interact || !ch)
            return false;
        __try {
            portal->BndEvt__BP_Aladdin_ArenaManagerSingleton_BP_InteractableComponent_K2Node_ComponentBoundEvent_2_OnInteractionEventDel__DelegateSignature(
                interact, ch);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool FireGenericPortalInteractionEventsSeh(SDK::ABP_Interactable_C* portal,
                                                      SDK::UHWInteractableComponent* interact,
                                                      SDK::AHWCharacter_Base* ch)
    {
        if (!portal || !interact || !ch)
            return false;

        bool fired = false;
        __try {
            portal->BndEvt__BP_Interactable_BP_InteractableComponent_K2Node_ComponentBoundEvent_0_OnInteractionEventDel__DelegateSignature(
                interact, ch);
            fired = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        __try {
            portal->BndEvt__BP_Interactable_BP_InteractableComponent_K2Node_ComponentBoundEvent_1_OnInteractionEventDel__DelegateSignature(
                interact, ch);
            fired = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        __try {
            portal->BndEvt__BP_Interactable_BP_InteractableComponent_K2Node_ComponentBoundEvent_2_OnInteractionEventDel__DelegateSignature(
                interact, ch);
            fired = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return fired;
    }

    static bool FireJoustPortalDelegateSeh(SDK::AActor* actor,
                                           SDK::UHWInteractableComponent* interact,
                                           SDK::AHWCharacter_Base* ch)
    {
        if (!actor || !interact || !ch)
            return false;

        struct DelegateParms
        {
            SDK::UHWInteractableComponent* Interactable;
            SDK::AActor* InteractInstigator;
        };

        DelegateParms parms{};
        parms.Interactable = interact;
        parms.InteractInstigator = ch;

        static const char* kDelegateNames[] = {
            "BndEvt__BP_Interactable_PortalGate_Joust_BP_InteractableComponent_K2Node_ComponentBoundEvent_0_OnInteractionEventDel__DelegateSignature",
            "BndEvt__BP_Interactable_PortalGate_BP_InteractableComponent_K2Node_ComponentBoundEvent_0_OnInteractionEventDel__DelegateSignature",
            "BndEvt__BP_Interactable_BP_InteractableComponent_K2Node_ComponentBoundEvent_0_OnInteractionEventDel__DelegateSignature",
            nullptr
        };

        __try {
            for (SDK::UStruct* structType = actor->Class; structType;
                 structType = structType->SuperStruct)
            {
                auto* cls = static_cast<SDK::UClass*>(structType);
                char className[160]{};
                if (!CopyClassNameSeh(cls, className, sizeof(className)))
                    continue;

                for (int i = 0; kDelegateNames[i]; ++i)
                {
                    SDK::UFunction* fn = cls->GetFunction(className, kDelegateNames[i]);
                    if (!fn)
                        continue;

                    auto flgs = fn->FunctionFlags;
                    fn->FunctionFlags |= 0x400;
                    actor->ProcessEvent(fn, &parms);
                    fn->FunctionFlags = flgs;
                    return true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return false;
    }

    static bool AbilityClassNameContainsSeh(SDK::UGameplayAbility* ability, const char* needle)
    {
        if (!ability || !needle || !*needle)
            return false;
        __try {
            if (!ability->Class)
                return false;
            return ClassNameContainsSeh(ability->Class, needle);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool TryActivateAbilityByClassNameSeh(SDK::UHWAbilitySystemComponent* hwAsc,
                                                 const char* classNeedle)
    {
        if (!hwAsc || !classNeedle || !*classNeedle)
            return false;

        SDK::UAbilitySystemComponent* asc = hwAsc;
        __try {
            const auto& items = hwAsc->ActivatableAbilities.Items;
            for (int i = 0; i < items.Num(); ++i)
            {
                if (!items.IsValidIndex(i))
                    continue;

                SDK::UGameplayAbility* ability = items[i].Ability;
                if (!ability || !Helpers::IsValidPtr(ability))
                    continue;
                if (!AbilityClassNameContainsSeh(ability, classNeedle))
                    continue;

                if (asc->TryActivateAbility(items[i].Handle, true))
                    return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return false;
    }

    static bool ConfirmTeleporterTargetingSeh(SDK::UGameplayAbility* ability)
    {
        if (!ability)
            return false;

        SDK::UFunction* fn = SDK::UHWGameplayAbility_Enhanced::StaticClass()->GetFunction(
            "HWGameplayAbility_Enhanced", "ConfirmTargeting");
        if (!fn)
            return false;

        __try {
            struct { bool bEndTargetingTask; } parms{ true };
            auto flgs = fn->FunctionFlags;
            fn->FunctionFlags |= 0x400;
            ability->ProcessEvent(fn, &parms);
            fn->FunctionFlags = flgs;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ConfirmActiveTeleporterInstanceSeh(SDK::UGameplayAbility* inst)
    {
        if (!inst || !Helpers::IsValidPtr(inst))
            return false;
        if (!AbilityClassNameContainsSeh(inst, "Teleporter"))
            return false;

        bool active = false;
        __try { active = inst->bIsActive; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

        if (!active)
            return false;
        return ConfirmTeleporterTargetingSeh(inst);
    }

    static bool TryConfirmActiveTeleporterSeh(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        if (!hwAsc)
            return false;

        __try {
            const auto& items = hwAsc->ActivatableAbilities.Items;
            for (int i = 0; i < items.Num(); ++i)
            {
                if (!items.IsValidIndex(i))
                    continue;

                const SDK::FGameplayAbilitySpec& spec = items[i];
                for (int j = 0; j < spec.NonReplicatedInstances.Num(); ++j)
                {
                    if (!spec.NonReplicatedInstances.IsValidIndex(j))
                        continue;
                    if (ConfirmActiveTeleporterInstanceSeh(spec.NonReplicatedInstances[j]))
                        return true;
                }
                for (int j = 0; j < spec.ReplicatedInstances.Num(); ++j)
                {
                    if (!spec.ReplicatedInstances.IsValidIndex(j))
                        continue;
                    if (ConfirmActiveTeleporterInstanceSeh(spec.ReplicatedInstances[j]))
                        return true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return false;
    }

    static bool TrySendGameplayEventSeh(SDK::UHWAbilitySystemComponent* hwAsc,
                                        const SDK::FGameplayTag& tag)
    {
        if (!hwAsc || tag.TagName.IsNone())
            return false;

        __try {
            SDK::FGameplayEventData payload{};
            payload.EventTag = tag;
            hwAsc->ServerSendGameplayEventToAvatar(tag, payload);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool TrySendMapTeleporterInvisEventsSeh(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        static const char* kTags[] = {
            "Maps.Gameplay.BaseTeleporter",
            "Event.Teleport.Map",
            "Event.MapTeleport",
            "Event.Teleport.BaseTeleporter",
            "Event.BaseTeleporter",
            "Maps.Gameplay.Teleporter",
            nullptr
        };

        for (int i = 0; kTags[i]; ++i)
        {
            SDK::FGameplayTag tag{};
            if (!RequestTag(kTags[i], tag))
                continue;
            if (TrySendGameplayEventSeh(hwAsc, tag))
                return true;
        }

        return false;
    }

    static bool PrimeInteractAbilityTargetSeh(SDK::UHWAbilitySystemComponent* hwAsc,
                                              SDK::UHWInteractableComponent* interact)
    {
        if (!hwAsc || !interact)
            return false;

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

                const uintptr_t abPtr = reinterpret_cast<uintptr_t>(ability);
                *reinterpret_cast<SDK::UHWInteractableComponent**>(abPtr + 0x0DF8) = interact;
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        return false;
    }

    static bool TrySendTowerTeleportGameplayEventSeh(SDK::UHWAbilitySystemComponent* hwAsc,
                                                     SDK::ABP_Interactable_TowerPortals_C* portal,
                                                     bool linkedMapPortal,
                                                     bool joustMatch)
    {
        if (!hwAsc || !portal)
            return false;

        __try {
            const SDK::FGameplayTag tag = portal->TeleportTag;
            if (!tag.TagName.IsNone())
                return TrySendGameplayEventSeh(hwAsc, tag);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

        static const char* kMapTags[] = {
            "Event.Teleport.Map",
            "Event.MapTeleport",
            "Event.Joust.Teleport",
            "Event.Teleport.PortalGate",
            "Event.PortalGate",
            nullptr
        };

        static const char* kBaseTags[] = {
            "Event.Teleport.BaseTeleporter",
            "Event.BaseTeleporter",
            "Event.Teleport.PortalGate",
            nullptr
        };

        const bool preferMapTags = linkedMapPortal || joustMatch;
        const char** primary = preferMapTags ? kMapTags : kBaseTags;
        const char** secondary = preferMapTags ? kBaseTags : kMapTags;

        for (int pass = 0; pass < 2; ++pass)
        {
            const char** tags = pass == 0 ? primary : secondary;
            for (int i = 0; tags[i]; ++i)
            {
                SDK::FGameplayTag tag{};
                if (!RequestTag(tags[i], tag))
                    continue;
                if (TrySendGameplayEventSeh(hwAsc, tag))
                    return true;
            }
        }

        return false;
    }

    static bool TrySendGenericTeleportGameplayEventSeh(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        static const char* kGenericTags[] = {
            "Event.Teleport.Map",
            "Event.MapTeleport",
            "Event.Joust.Teleport",
            "Event.Teleport.PortalGate",
            "Event.PortalGate",
            "Event.Teleport.BaseTeleporter",
            "Event.BaseTeleporter",
            nullptr
        };

        for (int i = 0; kGenericTags[i]; ++i)
        {
            SDK::FGameplayTag tag{};
            if (!RequestTag(kGenericTags[i], tag))
                continue;
            if (TrySendGameplayEventSeh(hwAsc, tag))
                return true;
        }

        return false;
    }

    static bool TrySendAladdinTeleportGameplayEventSeh(SDK::UHWAbilitySystemComponent* hwAsc)
    {
        static const char* kAladdinTags[] = {
            "Event.Aladdin.Teleport.Send",
            "Event.Aladdin.Teleport.Return",
            "Event.Aladdin.Lamp.Interact",
            "Event.Aladdin.Teleport",
            nullptr
        };

        for (int i = 0; kAladdinTags[i]; ++i)
        {
            SDK::FGameplayTag tag{};
            if (!RequestTag(kAladdinTags[i], tag))
                continue;
            if (TrySendGameplayEventSeh(hwAsc, tag))
                return true;
        }

        return false;
    }

    static bool RunBaseTeleporterInvisSteps(SDK::UHWAbilitySystemComponent* hwAsc,
                                            SDK::UHWInteractableComponent* interact)
    {
        if (!hwAsc)
            return false;

        if (interact)
            PrimeInteractAbilityTargetSeh(hwAsc, interact);

        bool triggered = false;
        if (TryActivateAbilityByClassNameSeh(hwAsc, "BaseTeleporter"))
            triggered = true;
        if (TryActivateAbilityByClassNameSeh(hwAsc, "HWGameplayAbility_Teleporter"))
            triggered = true;
        if (TrySendMapTeleporterInvisEventsSeh(hwAsc))
            triggered = true;
        if (TryConfirmActiveTeleporterSeh(hwAsc))
            triggered = true;
        return triggered;
    }

    static bool RunJoustInvisibilityChain(const PortalPick& pick, SDK::AHWCharacter_Base* ch,
                                          SDK::UHWAbilitySystemComponent* hwAsc)
    {
        if (!pick.actor || !pick.interact || !ch || !hwAsc)
            return false;

        // Prime interaction context only — do NOT fire portal delegates or GA_COM_Interact.
        // Those complete a real jungle gate teleport and apply GE_Portal_Cooldown_Joust.
        StartBpInteractionSeh(pick.interact, ch);
        PrimeInteractAbilityTargetSeh(hwAsc, pick.interact);
        return RunBaseTeleporterInvisSteps(hwAsc, pick.interact);
    }

    static bool RunInvisibilityChain(const PortalPick& pick, SDK::AHWCharacter_Base* ch,
                                     SDK::UHWAbilitySystemComponent* hwAsc)
    {
        if (!pick.actor || !pick.interact || !ch || !hwAsc)
            return false;

        bool anyStep = false;

        if (pick.kind == PortalKind::Tower)
        {
            SetTowerPortalInstigatorSeh(pick.towerPortal, ch);

            if (StartBpInteractionSeh(pick.interact, ch))
                anyStep = true;

            PrimeInteractAbilityTargetSeh(hwAsc, pick.interact);

            if (TryActivateInteractAbility(hwAsc))
                anyStep = true;

            if (FireTowerPortalInteractionEventSeh(pick.towerPortal, pick.interact, ch))
                anyStep = true;

            if (TrySendTowerTeleportGameplayEventSeh(hwAsc, pick.towerPortal, pick.linkedMapPortal,
                                                   IsJoustMatchSeh()))
                anyStep = true;
        }
        else if (pick.kind == PortalKind::JoustGate)
        {
            return RunJoustInvisibilityChain(pick, ch, hwAsc);
        }
        else if (pick.kind == PortalKind::Aladdin)
        {
            SetAladdinPortalInstigatorSeh(pick.aladdinPortal, ch);

            if (StartBpInteractionSeh(pick.interact, ch))
                anyStep = true;

            PrimeInteractAbilityTargetSeh(hwAsc, pick.interact);

            if (TryActivateInteractAbility(hwAsc))
                anyStep = true;

            if (FireAladdinPortalInteractionEventSeh(pick.aladdinPortal, pick.interact, ch))
                anyStep = true;

            if (TrySendAladdinTeleportGameplayEventSeh(hwAsc))
                anyStep = true;
        }
        else
        {
            if (StartBpInteractionSeh(pick.interact, ch))
                anyStep = true;

            PrimeInteractAbilityTargetSeh(hwAsc, pick.interact);

            if (TryActivateInteractAbility(hwAsc))
                anyStep = true;

            if (FireGenericPortalInteractionEventsSeh(pick.genericPortal, pick.interact, ch))
                anyStep = true;

            if (TrySendGenericTeleportGameplayEventSeh(hwAsc))
                anyStep = true;
        }

        return anyStep;
    }

    void Reset()
    {
    }

    void Tick()
    {
    }

    bool TryTeleportToLane()
    {
        SDK::AHWCharacter_Base* ch = Helpers::GetLocalChar();
        if (!ch)
            return false;

        SDK::UHWAbilitySystemComponent* hwAsc = GetAsc(ch);
        if (!hwAsc)
            return false;

        const bool joustMatch = IsJoustMatchSeh();

        if (joustMatch)
        {
            const std::vector<PortalPick> joustRanked = CollectRankedJoustPortals(ch);
            for (const PortalPick& pick : joustRanked)
            {
                if (!PortalReadyForChain(pick, joustMatch))
                    continue;
                if (RunJoustInvisibilityChain(pick, ch, hwAsc))
                    return true;
            }

            if (RunBaseTeleporterInvisSteps(hwAsc, nullptr))
                return true;

            return false;
        }

        const std::vector<PortalPick> towerRanked = CollectRankedTowerPortals(ch);
        for (const PortalPick& pick : towerRanked)
        {
            if (!PortalReadyForChain(pick, joustMatch))
                continue;
            if (RunInvisibilityChain(pick, ch, hwAsc))
                return true;
        }

        const PortalPick aladdinPick = PickBestAladdinPortal(ch);
        if (PortalReadyForChain(aladdinPick, joustMatch) &&
            RunInvisibilityChain(aladdinPick, ch, hwAsc))
            return true;

        const std::vector<SDK::ABP_Interactable_C*> genericPortals = CollectGenericPortals();
        for (SDK::ABP_Interactable_C* portal : genericPortals)
        {
            const PortalPick pick = BuildGenericPortalPick(portal, ch);
            if (!pick.actor || !pick.interact)
                continue;
            if (RunInvisibilityChain(pick, ch, hwAsc))
                return true;
        }

        return false;
    }

    void DrawMenu()
    {
        UITheme::BeginCard("tp_main", UITheme::Icon::Bolt, "Invisibility");

        ImGui::Spacing();
        if (UITheme::ActionButton("tp_invis", UITheme::Icon::Bolt, "Activate",
                                  UITheme::palette.accent, { 180.f, 36.f }))
        {
            TryTeleportToLane();
        }

        UITheme::EndCard();
    }
}
