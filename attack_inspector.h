#pragma once
#include "../core/sdk_helpers.h"
#include "../ui/theme.h"
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <utility>

// Included once, from sky_inspector.cpp. UI publishes requests only. All SDK
// reads/getters run under the existing WorldEnvironment window-dispatch lock.
namespace AttackInspector
{
    namespace Detail
    {
        constexpr int MaxSpecs=128,MaxCandidates=4,MaxInstances=4;
        constexpr int MaxAttacks=16,MaxConfigs=16,MaxChanges=100,MaxBytes=150000;
        constexpr uint64_t DurationMs=12000,PollMs=100;
        inline std::atomic_bool requested{false},busy{false},stop{false};
        inline std::mutex outputMutex;
        inline std::string report,status="Not recorded";
        struct Job
        {
            SDK::UWorld* World=nullptr;SDK::ULevel* Level=nullptr;
            SDK::AHWCharacter_Base* Pawn=nullptr;SDK::UHWAbilitySystemComponent* Asc=nullptr;
            int WorldIndex=-1,LevelIndex=-1,PawnIndex=-1,AscIndex=-1;
            uint64_t Started=0,LastPoll=0;int Samples=0,Changes=0,Candidates=0,Faults=0;
            bool Limited=false,Changed=false;
            SDK::UObject* Configs[MaxConfigs]{};int ConfigIndices[MaxConfigs]{},ConfigCount=0;
            std::string LastState,Text;
        };
        inline std::unique_ptr<Job> job;
        inline bool Live(SDK::UObject* o,bool allowDefault=false)
        {
            if(!Helpers::IsValidPtr(o)||!Helpers::IsValidPtr(o->Class))return false;
            const auto bad=static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed)|
                static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage)|
                static_cast<uint32_t>(SDK::EObjectFlags::NeedLoad)|static_cast<uint32_t>(SDK::EObjectFlags::NeedInitialization)|
                static_cast<uint32_t>(SDK::EObjectFlags::NeedPostLoad)|static_cast<uint32_t>(SDK::EObjectFlags::NeedPostLoadSubobjects);
            if((static_cast<uint32_t>(o->Flags)&bad)||(!allowDefault&&o->IsDefaultObject()))return false;
            auto* table=SDK::UObject::GObjects.GetTypedPtr();
            return Helpers::IsValidPtr(table)&&table->Num()>=0&&table->Num()<=8000000&&
                o->Index>=0&&o->Index<table->Num()&&table->GetByIndex(o->Index)==o;
        }
        inline std::string Name(const SDK::FName& n)
        {
            auto text=n.ToString();if(text.size()>140)text.resize(140);
            for(auto& c:text)if(static_cast<unsigned char>(c)<32)c=' ';
            return text;
        }
        inline std::string Label(SDK::UObject* o)
        {return Live(o,true)?Name(o->Name)+" ["+Name(o->Class->Name)+"]":"None/unreadable";}
        template<class Array> inline bool Valid(const Array& a,int cap)
        {return a.Num()>=0&&a.Num()<=cap&&a.Max()>=a.Num()&&a.Max()<=1048576&&(!a.Num()||Helpers::IsValidPtr(a.GetDataPtr()));}
        template<class Array> inline bool Same(const Array& a,const Array& b)
        {return a.GetDataPtr()==b.GetDataPtr()&&a.Num()==b.Num()&&a.Max()==b.Max();}
        inline bool HasTag(const SDK::FGameplayTagContainer& container,const char* value)
        {
            auto tags=container.GameplayTags;if(!Valid(tags,128))return false;
            for(int i=0;i<tags.Num();++i)if(Name(tags[i].TagName)==value)return true;
            return false;
        }
        inline bool Candidate(const SDK::FGameplayAbilitySpec& spec)
        {
            auto* a=spec.Ability;if(!Live(a,true))return false;
            return a->IsA(SDK::UHWAbility_Inhand::StaticClass())||
                HasTag(spec.DynamicAbilityTags,"Input.Fire.Ability.0")||HasTag(a->AbilityTags,"Ability.Type.Inhand");
        }
        inline void Append(Job& j,const std::string& text)
        {
            if(j.Text.size()+text.size()>static_cast<size_t>(MaxBytes)){j.Limited=true;return;}
            j.Text+=text;
        }
        inline void Config(Job& j,const SDK::UHWAbilityLevelConfig* config,const char* source,std::ostringstream& state)
        {
            auto* object=const_cast<SDK::UHWAbilityLevelConfig*>(config);
            if(!Live(object,true)||!object->IsA(SDK::UHWAbilityLevelConfig::StaticClass()))
            {state<<" config=unavailable";return;}
            state<<" config="<<Name(object->Name)<<"#"<<object->Index;
            auto attacks=config->Attacks;
            if(!Valid(attacks,256)){state<<" attacks=invalid";j.Changed=true;return;}
            // Compact values are included in the state signature, so in-place
            // count/period changes on the same config are not silently hidden.
            state<<" configuredRepeats=[";
            for(int i=0;i<attacks.Num()&&i<MaxAttacks;++i)
                state<<i<<":"<<attacks[i].AttackFireCount<<"/"<<attacks[i].RefirePeriod<<";";
            state<<"]";
            for(int i=0;i<j.ConfigCount;++i)if(j.Configs[i]==object&&j.ConfigIndices[i]==object->Index)return;
            if(j.ConfigCount>=MaxConfigs){j.Limited=true;return;}
            j.Configs[j.ConfigCount]=object;j.ConfigIndices[j.ConfigCount++]=object->Index;
            std::ostringstream out;out<<std::fixed<<std::setprecision(4);
            out<<"\nCONFIG source="<<source<<" | "<<Label(object)<<" | configuredLevel="<<config->Level
                <<" cooldownDuration="<<config->CooldownDuration<<" (configuration, NOT remaining cooldown)\n";
            const auto d=config->AbilityPhaseDurations;
            out<<"phase durations target/build/pre/fire/post/end="<<d.TargetingPhaseDuration<<"/"<<d.BuildUpPhaseDuration
                <<"/"<<d.PreFirePhaseDuration<<"/"<<d.FirePhaseDuration<<"/"<<d.PostFirePhaseDuration<<"/"<<d.EndingPhaseDuration<<"\n";
            out<<"attack rows="<<attacks.Num()<<"; detailed first "<<MaxAttacks<<"\n";
            if(attacks.Num()>MaxAttacks)j.Limited=true;
            for(int i=0;i<attacks.Num()&&i<MaxAttacks;++i)
            {
                const auto a=attacks[i];
                out<<"  row="<<i<<" tag="<<Name(a.ID.TagName)<<" settingId="<<a.AttackSettingId
                    <<" aimSettingId="<<a.AimSettingId<<" AttackFireCount="<<a.AttackFireCount<<" RefirePeriod="<<a.RefirePeriod
                    <<" commitCostsOnRefire="<<a.bShouldCommitCostsOnRefire<<" activeDuration="<<a.ActiveFireDuration
                    <<" trigger="<<static_cast<int>(a.ActivationTriggerBehavior)<<" triggerDelay="<<a.TriggerDelayTime
                    <<" cachedAim="<<a.bUseCachedAim<<" cachedTargets="<<a.bUseCachedTargets
                    <<" countsAsInhandForItems="<<a.bCountsAsInhandItemAttackForItems<<"\n";
            }
            if(!Same(attacks,config->Attacks)){j.Changed=true;out<<"  WARNING: attack array changed while reading\n";}
            out<<"Setting IDs are not assumed to be array indices. Config data is not proof of server acceptance.\n";
            Append(j,out.str());
        }
        inline void AuthoredConfig(Job& j,SDK::UGameplayAbility* ability,int specLevel,std::ostringstream& state)
        {
            if(!ability->IsA(SDK::UHWGameplayAbility::StaticClass()))return;
            auto* hw=static_cast<SDK::UHWGameplayAbility*>(ability);
            state<<" authoredSpecLevel="<<specLevel<<" singleLevel="<<hw->bUseSingleLevelConfig
                <<" singleLCMax="<<hw->SingleLC_AbilityMaxLevel;
            // Explicit-level native getter works with the spec object's authored
            // configuration. It is not GetCurrentLevelConfig on a CDO. Do not
            // manually traverse the SDK's defective sparse-map accessors.
            Config(j,hw->GetLevelConfig(specLevel),"SPEC explicit-level getter (authored/client; NOT live selection)",state);
        }
        inline void Instance(Job& j,SDK::UGameplayAbility* ability,const char* source,std::ostringstream& state)
        {
            if(!Live(ability)||!ability->IsA(SDK::UHWGameplayAbility::StaticClass()))return;
            auto* hw=static_cast<SDK::UHWGameplayAbility*>(ability);
            state<<"\n instance="<<Label(hw)<<" source="<<source<<" specModeOnInstance="
                <<static_cast<int>(hw->CurrentActivationInfo.ActivationMode);
            // Getter invokes generated ProcessEvent wrapper; no activation,
            // FireAttack, spawn, RPC, delegates or attack-field writes here.
            const auto id=hw->GetCurrentFiringInstanceId();
            state<<" firingId="<<id.AbilityHandle.Handle<<":"<<id.InstanceId;
            if(hw->IsA(SDK::UHWGameplayAbility_Enhanced::StaticClass()))
            {
                auto* enhanced=static_cast<SDK::UHWGameplayAbility_Enhanced*>(hw);
                state<<" phase="<<static_cast<int>(enhanced->AbilityPhase)
                    <<" repeatLockout="<<enhanced->AbilityRepeatActivationLockoutTime
                    <<" inputLockout="<<enhanced->AbilityInputActivationLockoutTime
                    <<" continuousLockout="<<enhanced->ContinousFireLockoutTime;
            }
            if(hw->IsA(SDK::UHWAbility_Inhand::StaticClass()))
            {
                auto* inhand=static_cast<SDK::UHWAbility_Inhand*>(hw);
                state<<" chainCurrent/next/last="<<static_cast<int>(inhand->CurrentInhandAttackChainIndex)
                    <<"/"<<static_cast<int>(inhand->NextInhandAttackChainIndex)<<"/"<<static_cast<int>(inhand->LastFiredInhandAttackChainIndex);
            }
            Config(j,hw->GetCurrentLevelConfig(),"LIVE INSTANCE getter (client-local only)",state);
        }
        inline bool Collect(Job& j)
        {
            auto* world=Helpers::GetWorld();auto* pc=Helpers::GetLocalPC();
            if(!Live(world)||!Live(pc)||!Live(world->PersistentLevel))
            {Append(j,"\nContext unavailable: enter a loaded match.\n");return false;}
            if(j.World&&(j.World!=world||j.WorldIndex!=world->Index||j.Level!=world->PersistentLevel||j.LevelIndex!=world->PersistentLevel->Index))
            {Append(j,"\nSTOP: world or level changed; recordings are not mixed.\n");return false;}
            auto* raw=pc->Pawn;
            if(!Live(raw)||!raw->IsA(SDK::AHWCharacter_Base::StaticClass()))
            {Append(j,"\nNo local character pawn; spawn first. Spectator targets are not inspected.\n");return false;}
            auto* pawn=static_cast<SDK::AHWCharacter_Base*>(raw);
            if(j.Pawn&&(j.Pawn!=pawn||j.PawnIndex!=pawn->Index))
            {Append(j,"\nSTOP: local pawn changed; recordings are not mixed.\n");return false;}
            auto* asa=pawn->AbilitySystemActor;
            if(!Live(asa)||!asa->IsA(SDK::AHWCharacterAbilitySystemActor::StaticClass())||!Live(asa->AbilitySystemComponent))
            {Append(j,"\nLocal ability system unavailable.\n");return false;}
            auto* asc=asa->AbilitySystemComponent;
            if(!asc->IsA(SDK::UHWAbilitySystemComponent::StaticClass()))return false;
            if(j.World)
            {
                if(j.World!=world||j.WorldIndex!=world->Index||j.Level!=world->PersistentLevel||j.LevelIndex!=world->PersistentLevel->Index||
                    j.Pawn!=pawn||j.PawnIndex!=pawn->Index||j.Asc!=asc||j.AscIndex!=asc->Index)
                {Append(j,"\nSTOP: world, level, pawn or ASC changed; recordings are not mixed.\n");return false;}
            }
            else
            {
                j.World=world;j.WorldIndex=world->Index;j.Level=world->PersistentLevel;j.LevelIndex=j.Level->Index;
                j.Pawn=pawn;j.PawnIndex=pawn->Index;j.Asc=asc;j.AscIndex=asc->Index;
                Append(j,"World: "+Label(world)+"\nPawn: "+Label(pawn)+"\nASC: "+Label(asc)+"\n");
            }
            auto specs=asc->ActivatableAbilities.Items;
            if(!Valid(specs,4096)){Append(j,"\nInvalid ability array; stopped.\n");return false;}
            if(specs.Num()>MaxSpecs)j.Limited=true;
            std::ostringstream state;state<<std::fixed<<std::setprecision(4);int candidates=0;
            for(int i=0;i<specs.Num()&&i<MaxSpecs;++i)
            {
                const auto spec=specs[i];if(!Candidate(spec))continue;
                if(candidates++>=MaxCandidates){j.Limited=true;break;}
                auto* ability=spec.Ability;
                state<<"\nSPEC "<<Label(ability)<<" handle="<<spec.Handle.Handle<<" level="<<spec.Level
                    <<" active="<<static_cast<int>(spec.ActiveCount)<<" input="<<static_cast<int>(spec.InputPressed)
                    <<" pendingRemove="<<static_cast<int>(spec.PendingRemove)<<" mode="<<static_cast<int>(spec.ActivationInfo.ActivationMode)
                    <<" execution="<<static_cast<int>(ability->NetExecutionPolicy)<<" security="<<static_cast<int>(ability->NetSecurityPolicy)
                    <<" instancing="<<static_cast<int>(ability->InstancingPolicy);
                if(spec.PendingRemove||spec.Level<0||spec.Level>256)
                {state<<" (getters skipped: pending removal or invalid spec level)";continue;}
                // Never use context-sensitive getters on a CDO as if it were a
                // live instance. Enumerate both arrays, deduplicate by pointer.
                SDK::UGameplayAbility* seen[MaxInstances]{};int count=0;
                auto visit=[&](const auto& list,const char* source)
                {
                    if(!Valid(list,64)){j.Limited=true;return;}
                    if(list.Num()>MaxInstances)j.Limited=true;
                    for(int n=0;n<list.Num()&&n<MaxInstances;++n)
                    {
                        auto* instance=list[n];bool duplicate=false;
                        for(int k=0;k<count;++k)if(seen[k]==instance)duplicate=true;
                        if(duplicate||!Live(instance)||instance->Class!=ability->Class)continue;
                        if(count>=MaxInstances){j.Limited=true;break;}
                        seen[count++]=instance;Instance(j,instance,source,state);
                    }
                };
                visit(spec.NonReplicatedInstances,"nonreplicated instance array");
                visit(spec.ReplicatedInstances,"replicated instance array (NOT server acknowledgement)");
                if(!count&&!ability->IsDefaultObject())Instance(j,ability,"spec object (non-CDO; activity not assumed)",state);
                if(!count&&ability->IsDefaultObject())state<<" liveInstance=NONE (CDO is not an active attack)";
                AuthoredConfig(j,ability,spec.Level,state);
                if(!Same(spec.NonReplicatedInstances,specs[i].NonReplicatedInstances)||!Same(spec.ReplicatedInstances,specs[i].ReplicatedInstances))j.Changed=true;
            }
            j.Candidates=(candidates<MaxCandidates?candidates:MaxCandidates);
            if(!candidates)state<<"No candidates matched exact Input.Fire.Ability.0 / Ability.Type.Inhand / typed Inhand; not proof no basic attack exists.";
            ++j.Samples;
            if(!Same(specs,asc->ActivatableAbilities.Items))j.Changed=true;
            const auto text=state.str();
            if(text!=j.LastState)
            {
                if(j.Changes>=MaxChanges){j.Limited=true;return false;}
                ++j.Changes;j.LastState=text;
                Append(j,"\n+"+std::to_string(GetTickCount64()-j.Started)+" ms LOCAL SAMPLE:"+text+"\n");
            }
            return !j.Limited;
        }
        inline bool CollectSafe(Job* j)
        {__try{return Collect(*j);}__except(EXCEPTION_EXECUTE_HANDLER){++j->Faults;return false;}}
        inline void Publish(const Job& j,const char* result)
        {
            std::ostringstream tail;tail<<"\nEND: "<<result<<" | samples="<<j.Samples<<" changes="<<j.Changes<<" lastCandidates="<<j.Candidates
                <<" configs="<<j.ConfigCount<<" faults="<<j.Faults<<" limitsReached="<<j.Limited<<" arraysChanged="<<j.Changed
                <<"\nSampling may miss shots/transitions. This is NOT a shot counter, server acknowledgement, damage confirmation or desync proof.\n";
            std::lock_guard<std::mutex> lock(outputMutex);report=j.Text+tail.str();status=result;
        }
    }
    inline bool Pending(){return Detail::requested.load()||Detail::busy.load();}
    // Called only while holding the same dispatch lock as ProcessOnWindowThread.
    inline void CancelPending()
    {
        Detail::requested=false;Detail::stop=false;
        if(Detail::job)Detail::Publish(*Detail::job,"Cancelled by dispatcher/session/unload");
        Detail::job.reset();Detail::busy=false;
    }
    inline void ProcessOnWindowThread()
    {
        using namespace Detail;
        try
        {
            if(stop.exchange(false))
            {
                requested=false;
                if(job)Publish(*job,"Stopped by user");
                else {std::lock_guard<std::mutex> lock(outputMutex);status="Queued recording cancelled";}
                job.reset();busy=false;return;
            }
            if(requested.exchange(false)&&!job)
            {
                job=std::make_unique<Job>();busy=true;stop=false;job->Started=GetTickCount64();job->LastPoll=job->Started-PollMs;
                job->Text="ALWAYSLOSE - Basic attack config observer v1 (READ ONLY)\nClient-local, non-atomic; NOT a server acknowledgement.\n"
                    "No activation, extra attacks, RPC, spawning, packet manipulation or writes to game fields.\n"
                    "Read-only native getters: GetLevelConfig(explicit spec level), plus GetCurrentLevelConfig/GetCurrentFiringInstanceId on non-CDO instances. Generated ProcessEvent wrappers are used.\n"
                    "Observe up to 12 seconds / about 10 Hz; max 128 specs, 4 candidates, 4 instances/spec, 16 attack rows/config, 16 configs, 100 changes, 150 KB.\n"
                    "Spec explicit-level config is authored/client data, NOT proof of the live level config. Count/period semantics and server validation are not inferred.\n";
                job->Text+="Start monotonic ms="+std::to_string(job->Started)+"\n";
            }
            if(!job)return;
            const auto now=GetTickCount64();
            if(now-job->Started>=DurationMs)
            {Publish(*job,"Recording finished");job.reset();busy=false;return;}
            if(now-job->LastPoll<PollMs)return;
            job->LastPoll=now;
            if(!CollectSafe(job.get())){Publish(*job,"Stopped: context, fault or budget; see report");job.reset();busy=false;return;}
            Publish(*job,"Recording - use normal basic attacks, then copy report");
        }
        catch(...)
        {
            job.reset();busy=false;requested=false;stop=false;
            std::lock_guard<std::mutex> lock(outputMutex);status="Recording failed (C++ exception); start a new recording";
        }
    }
    inline void DrawPanel()
    {
        using namespace Detail;
        UITheme::BeginCard("attack_config_observer",UITheme::Icon::Info,"Basic attack observer - read only");
        UITheme::InfoText("Records basic-attack config and local instance state for 12 seconds. Does not enable double shot or alter attack speed.");
        const bool running=Pending();ImGui::BeginDisabled(running);
        if(ImGui::Button("Record basic attacks (12 s)",{UITheme::Px(245.f),UITheme::Px(36.f)}))requested=true;
        ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(!running);
        if(ImGui::Button("Stop recording"))stop=true;
        ImGui::EndDisabled();
        std::string text,message;{std::lock_guard<std::mutex> lock(outputMutex);text=report;message=status;}
        ImGui::BeginDisabled(text.empty()||requested.load());
        if(ImGui::Button("Copy basic attack report"))ImGui::SetClipboardText(text.c_str());
        ImGui::EndDisabled();ImGui::TextWrapped("Status: %s",requested.load()?"Waiting for window dispatcher...":message.c_str());
        if(!text.empty()&&ImGui::TreeNode("Basic attack report preview"))
        {ImGui::BeginChild("attack_config_text",{0.f,UITheme::Px(260.f)},true);ImGui::TextUnformatted(text.c_str());ImGui::EndChild();ImGui::TreePop();}
        UITheme::InfoText("Start, close this menu, then perform several normal basic attacks. Copy afterwards. Firing IDs/phases are sampled client state, not confirmed hits.");
        UITheme::EndCard();
    }
}
