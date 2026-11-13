#include "time_dilation.h"
#include "time_dilation_logic.h"
#include "../core/sdk_helpers.h"
#include "../ui/theme.h"
#include <atomic>
#include <mutex>

namespace TimeDilationExperiment
{
    namespace
    {
        std::mutex s_mutex;
        Config s_config{};
        std::atomic_bool s_enabled{false},s_pending{false};
        const char* s_status="Disabled. Experimental client-local world time only.";
        float s_baseline=0.f,s_requested=0.f,s_observed=0.f;
        struct Identity
        {
            SDK::UObject* Pointer=nullptr;
            SDK::UClass* Class=nullptr;
            SDK::UObject* Outer=nullptr;
            SDK::FName Name{};
            int Index=-1;
        };
        struct Lease
        {
            Identity World{},Settings{};
            float Original=1.f,Written=1.f,Multiplier=1.f;
        } s_lease; // Window dispatcher only. SDK does not expose registry serials.
        void Publish(const char* text)
        { std::lock_guard<std::mutex> lock(s_mutex);s_status=text; }
        void Sample(float baseline,float requested,float observed)
        { std::lock_guard<std::mutex> lock(s_mutex);s_baseline=baseline;s_requested=requested;s_observed=observed; }
        bool Live(SDK::UObject* object)
        {
            if(!Helpers::IsValidPtr(object)||!Helpers::IsValidPtr(object->Class))return false;
            const auto bad=static_cast<unsigned>(SDK::EObjectFlags::ClassDefaultObject)|
                static_cast<unsigned>(SDK::EObjectFlags::BeginDestroyed)|
                static_cast<unsigned>(SDK::EObjectFlags::FinishDestroyed)|
                static_cast<unsigned>(SDK::EObjectFlags::MirroredGarbage);
            return !(static_cast<unsigned>(object->Flags)&bad);
        }
        Identity Capture(SDK::UObject* object)
        { return {object,object->Class,object->Outer,object->Name,object->Index}; }
        bool Same(const Identity& key)
        {
            // Resolve registry entry BEFORE dereferencing a cached pointer.
            auto* object=SDK::UObject::GObjects->GetByIndex(key.Index);
            return object==key.Pointer && Live(object) && object->Class==key.Class &&
                object->Outer==key.Outer && object->Name==key.Name;
        }
        bool Registered(SDK::UObject* object)
        { return Live(object)&&object->Index>=0&&SDK::UObject::GObjects->GetByIndex(object->Index)==object; }
        void Forget() { s_pending=false;s_lease=Lease{}; }
        void Restore()
        {
            if(!s_pending.load())return;
            if(!Same(s_lease.Settings)){Forget();return;}
            auto* settings=static_cast<SDK::AWorldSettings*>(s_lease.Settings.Pointer);
            const float current=settings->TimeDilation;
            Sample(s_lease.Original,s_lease.Written,current);
            if(!Logic::StillOwned(current,s_lease.Written))
            {
                // Never overwrite a newer server/game value or fight replication.
                Forget();Publish("External value preserved; this module no longer owns the field.");return;
            }
            settings->TimeDilation=s_lease.Original;
            if(settings->TimeDilation!=s_lease.Original)
            { Publish("Restore not confirmed locally. Keep DLL loaded and retry disable.");return; }
            Sample(s_lease.Original,s_lease.Written,settings->TimeDilation);
            Forget();
        }
        bool Context(SDK::UWorld** outWorld,SDK::AWorldSettings** outSettings)
        {
            auto* world=Helpers::GetWorld();auto* rawPC=Helpers::GetLocalPC();
            if(!Registered(world)||!Live(world->PersistentLevel)||!Live(world->GameState)||
                !world->GameState->IsA(SDK::AHWGameState::StaticClass()))return false;
            if(!Live(rawPC)||!rawPC->IsA(SDK::AHWPlayerController_Smite::StaticClass()))return false;
            auto* pc=static_cast<SDK::AHWPlayerController_Smite*>(rawPC);
            if(pc->bIsDeathSpectating||!Live(pc->PlayerState)||pc->PlayerState->bIsInactive||pc->PlayerState->bFromPreviousLevel)return false;
            auto* pawn=pc->Pawn;
            if(!Live(pawn)||!pawn->IsA(SDK::AHWCharacter_PlayableCharacter::StaticClass())||pawn->Controller!=pc)return false;
            auto* settings=world->PersistentLevel->WorldSettings;
            if(!Registered(settings)||!settings->IsA(SDK::AWorldSettings::StaticClass())||settings->Outer!=world->PersistentLevel)return false;
            *outWorld=world;*outSettings=settings;return true;
        }
        void Poll(HWND hwnd,bool suspending)
        {
            Config config=GetConfig();
            if(suspending||!config.Enabled)
            { Restore();if(!s_pending.load())Publish("Disabled; matching owned value restored or external value preserved.");return; }
            if(GetForegroundWindow()!=hwnd)
            { Disable();Restore();Publish("Disabled on focus loss; re-enable explicitly when ready.");return; }
            SDK::UWorld* world=nullptr;SDK::AWorldSettings* settings=nullptr;
            if(!Context(&world,&settings))
            { Disable();Restore();Publish("Disabled: owned playable pawn and valid WorldSettings required (not lobby/spectating).");return; }
            if(s_pending.load())
            {
                if(world!=s_lease.World.Pointer||!Same(s_lease.World)||settings!=s_lease.Settings.Pointer||!Same(s_lease.Settings))
                { Disable();Restore();Publish("World/settings changed; disabled. Re-enable manually in the new world.");return; }
                const float current=settings->TimeDilation;
                Sample(s_lease.Original,s_lease.Written,current);
                if(!Logic::StillOwned(current,s_lease.Written))
                { Disable();Forget();Publish("Game/server replaced the local value; disabled without reapplying.");return; }
                if(config.Multiplier==s_lease.Multiplier)return;
                Restore();if(s_pending.load())return;
            }
            if(config.Multiplier<=1.f)
            { Publish("Multiplier 1.00: original world time, no write.");return; }
            const float baseline=settings->TimeDilation;
            float desired=baseline;
            if(!Logic::Desired(baseline,config.Multiplier,settings->MinGlobalTimeDilation,settings->MaxGlobalTimeDilation,&desired))
            { Disable();Publish("Disabled: invalid time value or SDK world limits.");return; }
            Sample(baseline,desired,baseline);
            if(desired==baseline){Publish("No write: world limits leave the value unchanged.");return;}
            // Recheck current context and baseline immediately before writing. Not atomic with UE.
            if(Helpers::GetWorld()!=world||world->PersistentLevel->WorldSettings!=settings||settings->TimeDilation!=baseline)
            { Disable();Publish("Context/value changed before write; disabled.");return; }
            s_lease={Capture(world),Capture(settings),baseline,desired,config.Multiplier};
            s_pending=true; // Mark before writing so a partial failure retains restore work.
            settings->TimeDilation=desired;
            Sample(baseline,desired,settings->TimeDilation);
            if(!Logic::StillOwned(settings->TimeDilation,desired))
            { Disable();Restore();Publish("Write not retained locally; disabled.");return; }
            Publish("Local field applied. NOT proof of faster server movement or attacks.");
        }
    }
    Config GetConfig(){std::lock_guard<std::mutex> lock(s_mutex);return s_config;}
    void SetConfig(const Config& value)
    {
        Config next=value;next.Multiplier=Logic::Multiplier(next.Multiplier);
        std::lock_guard<std::mutex> lock(s_mutex);s_config=next;s_enabled=next.Enabled;
    }
    void Disable(){std::lock_guard<std::mutex> lock(s_mutex);s_config.Enabled=false;s_enabled=false;}
    bool NeedsService(){return s_enabled.load()||s_pending.load();}
    bool HasPendingRestore(){return s_pending.load();}
    void TickOnWindowThread(HWND hwnd,bool suspending)
    {
        if(!NeedsService())return;
        __try { Poll(hwnd,suspending); }
        __except(EXCEPTION_EXECUTE_HANDLER)
        { Disable();Publish("SDK access fault. Disabled; restore remains pending if owned. Do not force unload."); }
    }
    void DrawPanel()
    {
        Config config=GetConfig();bool changed=false;
        UITheme::BeginCard("time_dilation_experiment",UITheme::Icon::Visuals,"Experimental / Client world time");
        changed|=UITheme::CheckboxRow("Enable experimental time dilation",&config.Enabled);
        changed|=UITheme::SliderFloatRow("Time multiplier",&config.Multiplier,1.f,1.5f,"%.2fx");
        if(ImGui::Button("Disable and restore")){config.Enabled=false;changed=true;}
        if(changed)SetConfig(config);
        const char* status;float baseline,requested,observed;
        {std::lock_guard<std::mutex> lock(s_mutex);status=s_status;baseline=s_baseline;requested=s_requested;observed=s_observed;}
        ImGui::TextWrapped("Status: %s",status);
        ImGui::Text("Last local sample: original=%.3f requested=%.3f observed=%.3f",baseline,requested,observed);
        ImGui::Text("Pending owned field: %s",HasPendingRestore()?"YES":"NO");
        UITheme::InfoText("Client-side WorldSettings.TimeDilation only. No RPC, driver, packet manipulation or server acknowledgement. This may affect animations/timing without faster movement or damage.");
        UITheme::InfoText("Defaults OFF and is not saved to profiles. Disables on focus loss, invalid gameplay context, world change or external replacement. Restore never overwrites a newer game value. Test in practice first.");
        UITheme::EndCard();
    }
}
