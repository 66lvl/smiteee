#pragma once
// Experimental CLIENT-LOCAL attribute override. No RPC or server acknowledgement.
// Included only from world_environment.cpp and serviced by its window dispatcher.
#include "../core/sdk_helpers.h"
#include "../ui/theme.h"
#include <atomic>
#include <cmath>
#include <mutex>

namespace AttackSpeedExperiment
{
    namespace
    {
        std::mutex g_mutex;
        bool g_enabled = false;
        float g_requested = 1.5f;
        const char* g_status = "Disabled. Client-local experiment.";
        float g_originalSample = 0.f, g_requestedSample = 0.f, g_observedSample = 0.f;
        std::atomic_bool g_service{false}, g_pending{false};
        struct Identity {
            SDK::UObject* Pointer = nullptr; SDK::UClass* Class = nullptr;
            SDK::UObject* Outer = nullptr; SDK::FName Name{}; int Index = -1;
        };
        struct Lease {
            Identity World{}, Pawn{}, Asc{}, Attributes{};
            float Original = 0.f, OriginalBase = 0.f, Written = 0.f;
        } g_lease; // Window dispatcher only; SDK does not expose object serial numbers.
        bool Live(SDK::UObject* obj)
        {
            if(!Helpers::IsValidPtr(obj)||!Helpers::IsValidPtr(obj->Class))return false;
            const auto bad=static_cast<unsigned>(SDK::EObjectFlags::ClassDefaultObject)|
                static_cast<unsigned>(SDK::EObjectFlags::BeginDestroyed)|
                static_cast<unsigned>(SDK::EObjectFlags::FinishDestroyed)|
                static_cast<unsigned>(SDK::EObjectFlags::MirroredGarbage);
            auto* table=SDK::UObject::GObjects.GetTypedPtr();
            return !(static_cast<unsigned>(obj->Flags)&bad)&&Helpers::IsValidPtr(table)&&
                table->Num()>=0&&table->Num()<=8000000&&obj->Index>=0&&obj->Index<table->Num()&&table->GetByIndex(obj->Index)==obj;
        }
        Identity Capture(SDK::UObject* obj)
        {return {obj,obj->Class,obj->Outer,obj->Name,obj->Index};}
        bool Same(const Identity& id)
        {
            auto* table=SDK::UObject::GObjects.GetTypedPtr();
            if(!Helpers::IsValidPtr(table)||id.Index<0||id.Index>=table->Num()||table->Num()>8000000)return false;
            auto* obj=table->GetByIndex(id.Index);
            return obj==id.Pointer&&Live(obj)&&obj->Class==id.Class&&obj->Outer==id.Outer&&obj->Name==id.Name;
        }
        void Publish(const char* text){std::lock_guard<std::mutex> lock(g_mutex);g_status=text;}
        void Sample(float original,float requested,float observed)
        {std::lock_guard<std::mutex> lock(g_mutex);g_originalSample=original;g_requestedSample=requested;g_observedSample=observed;}
        void Disable(){std::lock_guard<std::mutex> lock(g_mutex);g_enabled=false;g_service=false;}
        void Forget(){g_lease=Lease{};g_pending=false;}
        bool Restore()
        {
            if(!g_pending.load())return true;
            if(!Same(g_lease.Attributes)){Forget();return true;}
            auto* attr=static_cast<SDK::UHWAttributeSet_Character_Base*>(g_lease.Attributes.Pointer);
            const float current=attr->AttackSpeed.CurrentValue;
            const float base=attr->AttackSpeed.BaseValue;
            Sample(g_lease.Original,g_lease.Written,current);
            if(!std::isfinite(current)||!std::isfinite(base)||current!=g_lease.Written||base!=g_lease.OriginalBase)
            {Forget();Publish("Game changed the attribute; newer value preserved.");return true;}
            attr->AttackSpeed.CurrentValue=g_lease.Original;
            if(attr->AttackSpeed.CurrentValue!=g_lease.Original)
            {Publish("Restore not confirmed. Keep DLL loaded and retry.");return false;}
            Sample(g_lease.Original,g_lease.Written,attr->AttackSpeed.CurrentValue);
            Forget();return true;
        }
        bool Context(SDK::UWorld** outWorld,SDK::AHWCharacter_PlayableCharacter** outPawn,
                     SDK::UHWAbilitySystemComponent** outAsc,SDK::UHWAttributeSet_Character_Base** outAttr)
        {
            auto* world=Helpers::GetWorld();auto* rawPC=Helpers::GetLocalPC();
            if(!Live(world)||!Live(world->PersistentLevel)||!Live(world->GameState)||
               !world->GameState->IsA(SDK::AHWGameState::StaticClass()))return false;
            if(!Live(rawPC)||!rawPC->IsA(SDK::AHWPlayerController_Smite::StaticClass()))return false;
            auto* pc=static_cast<SDK::AHWPlayerController_Smite*>(rawPC);
            if(pc->bIsDeathSpectating||!Live(pc->PlayerState)||pc->PlayerState->bIsInactive||pc->PlayerState->bFromPreviousLevel)return false;
            auto* rawPawn=pc->Pawn;
            if(!Live(rawPawn)||!rawPawn->IsA(SDK::AHWCharacter_PlayableCharacter::StaticClass())||rawPawn->Controller!=pc)return false;
            auto* pawn=static_cast<SDK::AHWCharacter_PlayableCharacter*>(rawPawn);
            auto* actor=pawn->AbilitySystemActor;
            if(!Live(actor)||!actor->IsA(SDK::AHWCharacterAbilitySystemActor::StaticClass()))return false;
            auto* asc=actor->AbilitySystemComponent;
            if(!Live(asc)||!asc->IsA(SDK::UHWAbilitySystemComponent::StaticClass()))return false;
            auto attrs=asc->SpawnedAttributes;
            if(attrs.Num()<0||attrs.Num()>128||attrs.Max()<attrs.Num()||attrs.Max()>1024||
               (attrs.Num()>0&&!Helpers::IsValidPtr(attrs.GetDataPtr())))return false;
            SDK::UHWAttributeSet_Character_Base* found=nullptr;
            for(int i=0;i<attrs.Num();++i)
            {
                auto* set=attrs[i];
                if(Live(set)&&set->IsA(SDK::UHWAttributeSet_Character_Base::StaticClass()))
                {if(found)return false;found=static_cast<SDK::UHWAttributeSet_Character_Base*>(set);}
            }
            if(!found||attrs.GetDataPtr()!=asc->SpawnedAttributes.GetDataPtr()||attrs.Num()!=asc->SpawnedAttributes.Num())return false;
            *outWorld=world;*outPawn=pawn;*outAsc=asc;*outAttr=found;return true;
        }
        void Poll(HWND hwnd,bool suspending)
        {
            bool enabled;float requested;
            {std::lock_guard<std::mutex> lock(g_mutex);enabled=g_enabled;requested=g_requested;}
            if(suspending||!enabled)
            {if(Restore())Publish("Disabled; owned value restored or newer value preserved.");return;}
            if(GetForegroundWindow()!=hwnd)
            {Disable();Restore();Publish("Disabled after focus loss; enable again manually.");return;}
            SDK::UWorld* world=nullptr;SDK::AHWCharacter_PlayableCharacter* pawn=nullptr;
            SDK::UHWAbilitySystemComponent* asc=nullptr;SDK::UHWAttributeSet_Character_Base* attr=nullptr;
            if(!Context(&world,&pawn,&asc,&attr))
            {Disable();Restore();Publish("Disabled: local character or a unique attribute set not available.");return;}
            if(g_pending.load())
            {
                if(world!=g_lease.World.Pointer||pawn!=g_lease.Pawn.Pointer||asc!=g_lease.Asc.Pointer||attr!=g_lease.Attributes.Pointer||
                   !Same(g_lease.World)||!Same(g_lease.Pawn)||!Same(g_lease.Asc)||!Same(g_lease.Attributes))
                {Disable();Restore();Publish("Character or attributes changed; re-enable manually.");return;}
                const float current=attr->AttackSpeed.CurrentValue;
                Sample(g_lease.Original,g_lease.Written,current);
                if(!std::isfinite(current)||current!=g_lease.Written||attr->AttackSpeed.BaseValue!=g_lease.OriginalBase)
                {Disable();Forget();Publish("Game changed attack speed; stopped rather than overriding newer value.");return;}
                if(requested==g_lease.Written)return;
                if(!Restore())return;
            }
            const float original=attr->AttackSpeed.CurrentValue,base=attr->AttackSpeed.BaseValue;
            if(!std::isfinite(original)||!std::isfinite(base)||original<0.f||original>10.f||base<0.f||base>10.f)
            {Disable();Publish("Disabled: invalid or unexpected attack-speed baseline.");return;}
            Sample(original,requested,original);
            if(requested==original){Publish("Requested value already matches; no write.");return;}
            // Re-resolve the local owned context immediately before writing.
            SDK::UWorld* w2=nullptr;SDK::AHWCharacter_PlayableCharacter* p2=nullptr;
            SDK::UHWAbilitySystemComponent* a2=nullptr;SDK::UHWAttributeSet_Character_Base* s2=nullptr;
            if(!Context(&w2,&p2,&a2,&s2)||w2!=world||p2!=pawn||a2!=asc||s2!=attr||
               attr->AttackSpeed.CurrentValue!=original||attr->AttackSpeed.BaseValue!=base)
            {Disable();Publish("Context/value changed before write; disabled.");return;}
            g_lease={Capture(world),Capture(pawn),Capture(asc),Capture(attr),original,base,requested};
            g_pending=true; // Preserve restore obligation if write faults.
            attr->AttackSpeed.CurrentValue=requested;
            Sample(original,requested,attr->AttackSpeed.CurrentValue);
            if(attr->AttackSpeed.CurrentValue!=requested)
            {Disable();Restore();Publish("Local write not retained; disabled.");return;}
            Publish("Local CurrentValue changed. Server attack rate NOT confirmed.");
        }
    }
    inline bool NeedsService(){return g_service.load()||g_pending.load();}
    inline bool HasPendingRestore(){return g_pending.load();}
    inline void RequestDisable(){Disable();}
    inline void TickOnWindowThread(HWND hwnd,bool suspending)
    {
        if(!NeedsService())return;
        __try {Poll(hwnd,suspending);}
        __except(EXCEPTION_EXECUTE_HANDLER)
        {Disable();Publish("SDK access fault; disable and wait for restoration before unload.");}
    }
    inline void DrawPanel()
    {
        bool enabled;float requested;const char* status;float original,written,observed;
        {std::lock_guard<std::mutex> lock(g_mutex);enabled=g_enabled;requested=g_requested;}
        UITheme::BeginCard("attack_speed_experiment",UITheme::Icon::Combat,"Experimental / Attack speed");
        bool changed=UITheme::CheckboxRow("Enable local attack-speed override",&enabled);
        changed|=UITheme::SliderFloatRow("Target attack speed",&requested,0.5f,3.0f,"%.2f");
        if(ImGui::Button("Disable attack speed and restore")){enabled=false;changed=true;}
        if(changed){std::lock_guard<std::mutex> lock(g_mutex);g_enabled=enabled;g_requested=std::isfinite(requested)?(requested<.5f?.5f:(requested>3.f?3.f:requested)):1.5f;g_service=enabled;}
        {std::lock_guard<std::mutex> lock(g_mutex);status=g_status;original=g_originalSample;written=g_requestedSample;observed=g_observedSample;}
        ImGui::TextWrapped("Status: %s",status);
        ImGui::Text("Local value: original=%.2f requested=%.2f observed=%.2f",original,written,observed);
        ImGui::Text("Restore pending: %s",HasPendingRestore()?"YES":"NO");
        UITheme::InfoText("Only the local AttackSpeed CurrentValue is written. BaseValue, server state and ability cooldowns are not changed; real attack rate is unconfirmed.");
        UITheme::InfoText("Off by default; not saved. Disables on focus loss, context change or a game update to the attribute. Restore is best effort. Test only in a permitted practice environment.");
        UITheme::EndCard();
    }
}
