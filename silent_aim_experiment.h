#pragma once
// Build-specific A/B experiment; mutation is opt-in and disabled initially.
// Shared target data is modified in probe mode; this can have side effects.
#include "../core/sdk_helpers.h"
#include "../core/game_state.h"
#include "../core/pawn_filter.h"
#include "../core/hooks.h"
#include <MinHook.h>
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <iomanip>

namespace SilentAimExperiment
{
    namespace Detail
    {
        constexpr uintptr_t AimRva=0x05CDE450, DataVtableRva=0x09CE5DE8;
        constexpr uint64_t MaxAgeMs=250;
        constexpr int MaxAbilities=32;
        using Fn=void* (__fastcall*)(void*,void*);
        struct AbilityIdentity { SDK::UGameplayAbility* ptr=nullptr; SDK::UClass* cls=nullptr; int index=-1; };
        struct Snapshot {
            SDK::APlayerController* pc=nullptr;
            SDK::AHWCharacter_Base* local=nullptr;
            SDK::FVector target{};
            AbilityIdentity abilities[MaxAbilities]{};
            int abilityCount=0;
            uint64_t at=0;
            bool valid=false;
        };
        // Fixed-size, best-effort observations only. No pointers exported.
        struct TraceItem { unsigned long long ms{}; int kind{}, objectIndex{}, instanceId{}, sourceType{}, targetType{}, phase{}, writeResult{};
            SDK::FVector source{}, endpoint{}, endpointAfter{}, selected{}, velocity{}, spawn{};
            SDK::FQuat sourceRotation{}, targetRotation{}, spawnRotation{}; };
        constexpr int MaxTrace=256;
        inline TraceItem trace[MaxTrace]{}; inline int traceCount=0;
        inline std::mutex traceMutex; inline std::atomic<unsigned> traceDropped{0}, traceProjCandidates{0};
        inline std::atomic<uint64_t> traceStart{0};
        inline void SaveTrace(const TraceItem& t) {
            if(!traceMutex.try_lock()){traceDropped.fetch_add(1);return;}
            if(traceCount<MaxTrace)trace[traceCount++]=t;else traceDropped.fetch_add(1);
            traceMutex.unlock();
        }
        inline std::atomic_bool enabled{false}, probeEnabled{false};
        inline std::atomic<unsigned> writesApplied{0},writesRejected{0};
        // Accessed only on the window dispatcher. Distinguish pooled instances.
        struct SeenShot {int index=-1,id=-1;};
        inline SeenShot seenShots[128]{};inline int seenShotCount=0;inline std::mutex seenMutex;
        inline bool FirstShotSample(int index,int id) {
            std::lock_guard<std::mutex> lock(seenMutex);
            for(int i=0;i<seenShotCount;i++)if(seenShots[i].index==index && seenShots[i].id==id)return false;
            if(seenShotCount>=128)return false;seenShots[seenShotCount++]={index,id};return true;
        }
        inline std::atomic<unsigned> changed{0};
        // Counters report local control flow only, never server acceptance.
        enum class ScanState : int { Waiting, FocusOrContext, Controller, Pawn, Dead,
            AbilitySystem, BadSpecs, NoAbilities, Origin, NoEnemy, ContextChanged, Ready, Fault };
        inline std::atomic<int> scanState{static_cast<int>(ScanState::Waiting)};
        inline std::atomic<unsigned> hookCalls{0}, cacheSkipped{0}, abilitySkipped{0};
        inline std::atomic<unsigned> handleSkipped{0}, typeSkipped{0}, invalidAim{0}, writeSkipped{0}, accessFault{0};
        inline bool ScanFail(ScanState reason) { scanState.store(static_cast<int>(reason)); return false; }
        inline const char* ScanLabel(int state) {
            switch(static_cast<ScanState>(state)) {
            case ScanState::Waiting: return "waiting for poll";
            case ScanState::FocusOrContext: return "not playable or game not focused";
            case ScanState::Controller: return "local controller unavailable";
            case ScanState::Pawn: return "local character unavailable";
            case ScanState::Dead: return "local character dead";
            case ScanState::AbilitySystem: return "local ability system unavailable";
            case ScanState::BadSpecs: return "ability list invalid";
            case ScanState::NoAbilities: return "no matching active basic ability instances";
            case ScanState::Origin: return "local position invalid";
            case ScanState::NoEnemy: return "no eligible enemy hero in range";
            case ScanState::ContextChanged: return "local character changed";
            case ScanState::Ready: return "local ability and enemy found";
            case ScanState::Fault: return "access fault during scan";
            default: return "unknown";
            }
        }
        inline std::atomic<uint64_t> lastPoll{0};
        inline std::mutex cacheMutex;
        inline Snapshot cache{};
        inline Fn original=nullptr;
        inline uintptr_t base=0;
        inline bool installed=false; // UI thread only
        inline const char* status="Disabled"; // UI thread only
        inline float rangeMeters=40.f; // UI thread only; published separately
        inline std::atomic<float> rangePublished{40.f};

        inline bool Live(SDK::UObject* obj,bool allowDefault=false)
        {
            if(!Helpers::IsValidPtr(obj)||!Helpers::IsValidPtr(obj->Class)||(!allowDefault&&obj->IsDefaultObject()))return false;
            const auto bad=static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
            if(static_cast<uint32_t>(obj->Flags)&bad)return false;
            auto* table=SDK::UObject::GObjects.GetTypedPtr();
            return Helpers::IsValidPtr(table)&&table->Num()>=0&&table->Num()<=8000000&&obj->Index>=0&&obj->Index<table->Num()&&table->GetByIndex(obj->Index)==obj;
        }
        template<class Array> inline bool ValidArray(const Array& a,int cap)
        { return a.Num()>=0&&a.Num()<=cap&&a.Max()>=a.Num()&&a.Max()<=1048576&&(!a.Num()||Helpers::IsValidPtr(a.GetDataPtr())); }
        inline bool Finite(const SDK::FVector& v)
        {return std::isfinite(v.X)&&std::isfinite(v.Y)&&std::isfinite(v.Z);}
        inline void AddAbility(Snapshot& out, SDK::UGameplayAbility* a, SDK::UClass* expectedClass)
        {
            if(out.abilityCount>=MaxAbilities||!Live(a)||a->Class!=expectedClass)return;
            for(int i=0;i<out.abilityCount;++i)if(out.abilities[i].ptr==a)return;
            out.abilities[out.abilityCount++]={a,a->Class,a->Index};
        }
        // Called on the existing window dispatcher, at most every 100ms.
        // Build a complete snapshot first; never publish partially collected data.
        inline bool Collect(Snapshot& out)
        {
            if(!enabled.load()||!GameState::IsPlayable()||!Hooks::g_hWnd||GetForegroundWindow()!=Hooks::g_hWnd)return ScanFail(ScanState::FocusOrContext);
            auto* pc=Helpers::GetLocalPC();
            if(!Live(pc))return ScanFail(ScanState::Controller);
            auto* raw=pc->Pawn;
            if(!Live(raw)||!raw->IsA(SDK::AHWCharacter_Base::StaticClass())||raw->Controller!=pc)return ScanFail(ScanState::Pawn);
            auto* local=static_cast<SDK::AHWCharacter_Base*>(raw);
            if(Helpers::IsDead(local))return ScanFail(ScanState::Dead);
            auto* asa=local->AbilitySystemActor;
            if(!Live(asa)||!Live(asa->AbilitySystemComponent))return ScanFail(ScanState::AbilitySystem);
            auto* asc=asa->AbilitySystemComponent;
            auto specs=asc->ActivatableAbilities.Items;
            if(!ValidArray(specs,2048))return ScanFail(ScanState::BadSpecs);
            for(int i=0;i<specs.Num()&&i<128;++i) {
                const auto spec=specs[i];
                if(spec.PendingRemove||!Live(spec.Ability,true)) continue;
                // CDO is used only to classify the spec, never as the firing instance.
                // Common Inhand class or inhand/fire tags; does not prove all gods use this path.
                if(!AttackInspector::Detail::Candidate(spec))continue;
                auto gather=[&](const auto& arr){
                    if(!ValidArray(arr,64))return;
                    for(int j=0;j<arr.Num()&&j<16;++j)AddAbility(out,arr[j],spec.Ability->Class);
                };
                gather(spec.NonReplicatedInstances);gather(spec.ReplicatedInstances);
                AddAbility(out,spec.Ability,spec.Ability->Class);
            }
            if(!out.abilityCount)return ScanFail(ScanState::NoAbilities);
            const SDK::FVector origin=Helpers::GetLocation(local);
            if(!Finite(origin))return ScanFail(ScanState::Origin);
            double best=static_cast<double>(rangePublished.load());best=best*100.0;best*=best;
            bool found=false;
            for(auto* pawn:Helpers::GetAllPawns()) {
                if(!Live(pawn)||pawn==local||!PawnFilter::IsHeroPawn(pawn)||!PawnFilter::IsAlivePawn(pawn)||!Helpers::IsHostileToLocal(local,pawn))continue;
                const auto p=Helpers::GetLocation(pawn);if(!Finite(p))continue;
                const double dx=p.X-origin.X,dy=p.Y-origin.Y,dz=p.Z-origin.Z;
                const double d=dx*dx+dy*dy+dz*dz;
                if(std::isfinite(d)&&d>1.0&&d<best){best=d;out.target=p;found=true;}
            }
            if(!found)return ScanFail(ScanState::NoEnemy);
            if(pc->Pawn!=local||local->Controller!=pc)return ScanFail(ScanState::ContextChanged);
            out.pc=pc;out.local=local;out.at=GetTickCount64();out.valid=true;
            scanState.store(static_cast<int>(ScanState::Ready));return true;
        }
        // CDO-aware validity for spec classification.
        inline bool CollectSafe(Snapshot* out)
        { __try { return Collect(*out); } __except(EXCEPTION_EXECUTE_HANDLER) { return ScanFail(ScanState::Fault); } }
        inline bool Writable(void* address,size_t size)
        {
            MEMORY_BASIC_INFORMATION info{};
            if(!VirtualQuery(address,&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
            const DWORD p=info.Protect&0xff;
            if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
            const auto start=reinterpret_cast<uintptr_t>(address),end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
            return start<=end&&size<=end-start;
        }
        // Exception handling cannot guarantee recovery from process termination or
        // concurrent engine writes. No arbitrary target-data casts are attempted.
        inline bool ApplySafe(void* taskPtr,void* result,const Snapshot* snap)
        {
            __try {
                if(!taskPtr||!result||!snap||!snap->valid||!snap->pc||!snap->local||
                   snap->pc->Pawn!=snap->local||snap->local->Controller!=snap->pc)return false;
                auto* task=static_cast<SDK::UHWAbilityTask_FireAttack*>(taskPtr);
                auto* ability=task->Ability;bool ours=false;
                for(int i=0;i<snap->abilityCount;++i) {
                    const auto& a=snap->abilities[i];
                    if(ability==a.ptr&&ability&&ability->Class==a.cls&&ability->Index==a.index){ours=true;break;}
                }
                if(!ours){abilitySkipped.fetch_add(1);return false;}
                // 1641736 handle layout verified statically and seen in the trace.
                const auto h=reinterpret_cast<uintptr_t>(result);
                const int count=*reinterpret_cast<const int*>(h+0x20);
                if(count!=1){handleSkipped.fetch_add(1);return false;}
                auto storage=*reinterpret_cast<const uintptr_t*>(h+0x18);
                if(!storage)storage=h+0x08;
                const auto item=*reinterpret_cast<const uintptr_t*>(storage);
                if(!item||*reinterpret_cast<const uintptr_t*>(item)!=base+DataVtableRva){typeSkipped.fetch_add(1);return false;}
                auto* data=reinterpret_cast<SDK::FHWTargetData_LocationInfo*>(item);
                if(!data->bIsValidAim||data->TargetLocation.LocationType!=SDK::EGameplayAbilityTargetingLocationType::LiteralTransform){invalidAim.fetch_add(1);return false;}
                // Baseline records unchanged values. Probe repeats the earlier translation-only
                // experiment; target data may be shared with other engine code.
                const auto& src=data->SourceLocation; auto& dst=data->TargetLocation;
                TraceItem t{};t.ms=GetTickCount64()-traceStart.load();t.kind=1;
                t.phase=probeEnabled.load(std::memory_order_acquire)?1:0;
                t.sourceType=static_cast<int>(src.LocationType);t.targetType=static_cast<int>(dst.LocationType);
                t.source=src.LiteralTransform.Translation;t.sourceRotation=src.LiteralTransform.Rotation;
                t.endpoint=dst.LiteralTransform.Translation;t.targetRotation=dst.LiteralTransform.Rotation;
                t.selected=snap->target;t.endpointAfter=t.endpoint;
                if(!Finite(t.source)||!Finite(t.endpoint)||!Finite(t.selected)){writeSkipped.fetch_add(1);return false;}
                if(t.phase) {
                    auto* dest=&dst.LiteralTransform.Translation;
                    if(Writable(dest,sizeof(*dest))) {
                        *dest=t.selected;
                        t.endpointAfter=*dest;t.writeResult=1;writesApplied.fetch_add(1);
                    } else {t.writeResult=2;writesRejected.fetch_add(1);}
                }
                SaveTrace(t);return true;
            } __except(EXCEPTION_EXECUTE_HANDLER) { accessFault.fetch_add(1);return false; }
        }
        inline void* __fastcall Detour(void* task,void* out)
        {
            // The original always runs exactly once; disabled/unavailable paths
            // leave its returned aim unchanged.
            void* result=original(task,out);
            if(!enabled.load(std::memory_order_acquire))return result;
            hookCalls.fetch_add(1);
            if(!result||!Hooks::g_hWnd||GetForegroundWindow()!=Hooks::g_hWnd){cacheSkipped.fetch_add(1);return result;}
            if(!cacheMutex.try_lock()){cacheSkipped.fetch_add(1);return result;}
            const Snapshot copy=cache;
            cacheMutex.unlock();
            const auto now=GetTickCount64();
            if(!copy.valid||now<copy.at||now-copy.at>MaxAgeMs){cacheSkipped.fetch_add(1);return result;}
            if(ApplySafe(task,result,&copy))changed.fetch_add(1,std::memory_order_relaxed);
            return result;
        }
        // Poll only bounded loaded-level actor arrays, on existing window dispatcher.
        // Inhand + a matching instigator/owner is a local-client indication only.
        inline void InspectProjectile(SDK::AActor* actor, SDK::APawn* local, uint64_t now)
        {
            __try {
                if(!actor||!Helpers::IsValidPtr(actor)||!actor->IsA(SDK::AHWProjectile::StaticClass()))return;
                traceProjCandidates.fetch_add(1);
                auto* p=static_cast<SDK::AHWProjectile*>(actor);
                if(!p->bIsInhand||p->bDormantWhilePooled)return;
                if(p->Instigator!=local && p->Owner!=local && p->ProjectileInstigator.Get()!=local)return;
                auto* root=p->RootComponent;
                if(!Live(root))return;
                TraceItem t{};t.ms=now-traceStart.load();t.kind=2;t.objectIndex=p->Index;t.instanceId=p->ProjectileInstanceId;
                t.phase=probeEnabled.load(std::memory_order_acquire)?1:0;
                t.spawn=p->OriginalSpawnTransform.Translation;t.spawnRotation=p->OriginalSpawnTransform.Rotation;t.velocity=root->ComponentVelocity;
                if(!Finite(t.spawn)||!Finite(t.velocity))return;
                const double speedSq=t.velocity.X*t.velocity.X+t.velocity.Y*t.velocity.Y+t.velocity.Z*t.velocity.Z;
                if(!std::isfinite(speedSq)||speedSq<1.0||!FirstShotSample(t.objectIndex,t.instanceId))return;
                SaveTrace(t);
            } __except(EXCEPTION_EXECUTE_HANDLER) { accessFault.fetch_add(1); }
        }
        inline void PollProjectiles(Snapshot* snap)
        {
            __try {
                if(!snap||!snap->valid||!snap->local)return;
                auto* world=Helpers::GetWorld();if(!Live(world)||!Live(world->PersistentLevel))return;
                auto levels=world->Levels;if(!ValidArray(levels,4096))return;
                SDK::ULevel* seen[32]{};int count=0,scanned=0;
                auto visit=[&](SDK::ULevel* level) {
                    if(!Live(level))return;for(int i=0;i<count;i++)if(seen[i]==level)return;
                    if(count>=32)return;seen[count++]=level;auto actors=level->Actors;
                    if(!ValidArray(actors,200000))return;
                    for(int j=0;j<actors.Num()&&scanned<25000;j++,scanned++)InspectProjectile(actors[j],snap->local,GetTickCount64());
                };
                visit(world->PersistentLevel);
                for(int i=0;i<levels.Num()&&i<32&&scanned<25000;i++)visit(levels[i]);
            } __except(EXCEPTION_EXECUTE_HANDLER) { accessFault.fetch_add(1); }
        }
        inline std::string Report() {
            std::ostringstream out;out<<std::fixed<<std::setprecision(2);
            out<<"RANGED A/B TRACE (probe changes shared aim data) build candidate 1641736\n"
               <<"Client snapshots only; not server acceptance, not a hit/shot counter.\n"
               <<"BASE=unchanged, PROBE=old endpoint-only experiment. Projectile phase is phase at observation, not launch.\n"
               <<"Aim Source/Target literal coordinates are meaningful only when location type is 0.\n"
               <<"Projectile ownership and inhand flag are hints; 100ms polling may miss events.\n"
               <<"Times ms since enabling; projectile_candidates="<<traceProjCandidates.load()<<" dropped="<<traceDropped.load()
               <<" writes_applied="<<writesApplied.load()<<" writes_rejected="<<writesRejected.load()<<"\n";
            auto vec=[&](const SDK::FVector& v){out<<"("<<v.X<<","<<v.Y<<","<<v.Z<<")";};
            auto quat=[&](const SDK::FQuat& q){out<<"("<<q.X<<","<<q.Y<<","<<q.Z<<","<<q.W<<")";};
            std::lock_guard<std::mutex> lock(traceMutex);
            for(int i=0;i<traceCount;i++) {const auto& t=trace[i];out<<"t="<<t.ms<<" phase_at_observation="<<(t.phase?"PROBE":"BASE")<<" ";
                if(t.kind==1){out<<"AIM source_type="<<t.sourceType<<" target_type="<<t.targetType<<" write="<<(t.writeResult==1?"applied":t.writeResult==2?"rejected":"none")<<" source=";vec(t.source);out<<" source_quat=";quat(t.sourceRotation);out<<" endpoint_before=";vec(t.endpoint);out<<" endpoint_after=";vec(t.endpointAfter);out<<" target_quat=";quat(t.targetRotation);out<<" chosen_enemy=";vec(t.selected);}
                else {out<<"PROJECTILE object_index="<<t.objectIndex<<" instance="<<t.instanceId<<" spawn=";vec(t.spawn);out<<" spawn_quat=";quat(t.spawnRotation);out<<" first_observed_velocity=";vec(t.velocity);}
                out<<"\n";
            }
            return out.str();
        }
        inline void* ResolveCandidate(int* matches)
        {
            __try {
                const auto image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                if(!image)return nullptr;
                const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
                if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||dos->e_lfanew>0x100000)return nullptr;
                const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+dos->e_lfanew);
                if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||
                   nt->FileHeader.NumberOfSections==0||nt->FileHeader.NumberOfSections>96)return nullptr;
                const size_t imageSize=nt->OptionalHeader.SizeOfImage;
                if(DataVtableRva+8>imageSize)return nullptr;
                const auto* sec=IMAGE_FIRST_SECTION(nt);
                static const unsigned char sig[]={
                    0x40,0x55,0x53,0x56,0x48,0x8d,0x6c,0x24,0xb9,0x48,0x81,0xec,0xb0,0,0,0,0x48,0x8b,0x05,
                    0,0,0,0,0x48,0x33,0xc4,0x48,0x89,0x45,0x3f,0xc6,0x02,0x00,0x48,0x8b,0xda
                };
                uintptr_t found=0;int n=0;
                for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){
                    if(!(sec[i].Characteristics&IMAGE_SCN_CNT_CODE)||!(sec[i].Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
                    const size_t start=sec[i].VirtualAddress,len=sec[i].Misc.VirtualSize;
                    if(start>=imageSize||len>imageSize-start||len<sizeof(sig))continue;
                    auto* p=reinterpret_cast<const unsigned char*>(image+start);
                    for(size_t j=0;j<=len-sizeof(sig);++j){
                        if(p[j]!=sig[0]||std::memcmp(p+j,sig,19)||std::memcmp(p+j+23,sig+23,sizeof(sig)-23))continue;
                        found=image+start+j;if(++n>1){*matches=n;return nullptr;}
                    }
                }
                *matches=n;
                if(n!=1||found-image!=AimRva)return nullptr;
                // Verify the vtable's first slot points to the build's known destructor.
                if(*reinterpret_cast<const uintptr_t*>(image+DataVtableRva)!=image+0x05BD0AC0)return nullptr;
                base=image;return reinterpret_cast<void*>(found);
            } __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
        }
        inline bool Install()
        {
            if(installed)return true;
            int matches=0;void* target=ResolveCandidate(&matches);
            if(!target){status="Unavailable: game signature/type mismatch (1641736 only)";return false;}
            auto result=MH_CreateHook(target,reinterpret_cast<void*>(&Detour),reinterpret_cast<void**>(&original));
            if(result!=MH_OK){status="Hook creation failed. Restart without the aim trace hook.";return false;}
            result=MH_EnableHook(target);
            if(result!=MH_OK){MH_RemoveHook(target);original=nullptr;status="Hook enable failed";return false;}
            installed=true;return true;
        }
    }
    inline bool NeedsService(){return Detail::enabled.load();}
    inline void Disable()
    {
        Detail::enabled.store(false,std::memory_order_release);
        Detail::probeEnabled.store(false,std::memory_order_release);
        std::lock_guard<std::mutex> lock(Detail::cacheMutex);Detail::cache=Detail::Snapshot{};
    }
    inline void TickOnWindowThread()
    {
        using namespace Detail;
        if(!enabled.load())return;
        const uint64_t now=GetTickCount64();
        if(now-lastPoll.load()<100)return;
        lastPoll.store(now);
        Snapshot next{};
        const bool valid=CollectSafe(&next);
        if(valid)PollProjectiles(&next);
        if(!valid)next=Snapshot{};
        std::lock_guard<std::mutex> lock(cacheMutex);cache=next;
    }
    inline void DrawPanel()
    {
        using namespace Detail;
        UITheme::BeginCard("ranged_ab_experiment",UITheme::Icon::Info,"Ranged A/B experiment (1641736)");
        bool on=enabled.load();
        if(UITheme::CheckboxRow("Record ranged trace",&on)){
            if(on){if(Install()){
                changed.store(0);hookCalls.store(0);cacheSkipped.store(0);abilitySkipped.store(0);
                handleSkipped.store(0);typeSkipped.store(0);invalidAim.store(0);writeSkipped.store(0);accessFault.store(0);
                scanState.store(static_cast<int>(ScanState::Waiting));lastPoll.store(0);
                {std::lock_guard<std::mutex> lock(traceMutex);traceCount=0;}
                {std::lock_guard<std::mutex> lock(seenMutex);seenShotCount=0;}
                probeEnabled.store(false);writesApplied.store(0);writesRejected.store(0);
                traceDropped.store(0);traceProjCandidates.store(0);traceStart.store(GetTickCount64());
                enabled.store(true,std::memory_order_release);status="Recording; BASE mode by default";}}
            else {Disable();status="Disabled; hook stays installed until DLL unload";}
        }
        bool probe=probeEnabled.load();
        ImGui::BeginDisabled(!enabled.load());
        if(UITheme::CheckboxRow("Enable endpoint probe (changes game state)",&probe))probeEnabled.store(probe,std::memory_order_release);
        ImGui::EndDisabled();
        if(UITheme::SliderFloatRow("Maximum target range",&rangeMeters,5.f,60.f,"%.0f m"))rangePublished.store(rangeMeters);
        ImGui::TextWrapped("%s | Aim samples: %u",status,changed.load());
        if(ImGui::Button("Copy ranged trace")){const auto text=Report();ImGui::SetClipboardText(text.c_str());}
        ImGui::TextWrapped("Scan: %s",ScanLabel(scanState.load()));
        ImGui::TextWrapped("Diagnostic calls=%u cache=%u ability=%u handle=%u type=%u aim=%u write=%u fault=%u",
            hookCalls.load(),cacheSkipped.load(),abilitySkipped.load(),handleSkipped.load(),typeSkipped.load(),
            invalidAim.load(),writeSkipped.load(),accessFault.load());
        UITheme::InfoText("BASE is read-only; endpoint probe writes shared game data and may have side effects. No hit or server confirmation. Hook remains until unload.");
        UITheme::EndCard();
    }
}
