#pragma once
// Opt-in read-only diagnostic for the provided 1641736 executable only.
// No attack / target data is modified. Hooking can trigger anti-cheat.
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <cstdint>
#include <cstring>

namespace BasicAimTrace
{
    namespace Detail
    {
        using Fn = void* (__fastcall*)(void*, void*);
        struct Sample {
            unsigned long long ms;
            unsigned long tid;
            uintptr_t task, ability, abilityClass, targetType;
            int count, readError;
        };
        constexpr int PerPhase=256;
        constexpr int MaxSamples=2*PerPhase;
        inline Fn original=nullptr;
        inline std::atomic<bool> recording{false};
        inline std::atomic<unsigned long long> startMs{0}, endMs{0};
        inline std::atomic<unsigned> dropped{0};
        inline std::mutex mutex;
        inline Sample samples[MaxSamples]{};
        inline int baselineCount=0, attackCount=0;
        inline bool installed=false;
        inline uintptr_t imageBase=0;
        inline std::string status="Not started";

        // Byte layout is inferred only for build 1641736; every read is guarded.
        // The callback never writes to these objects or calls game APIs.
        inline bool ReadSample(void* task, void* handle, Sample* out)
        {
            __try {
                if(!task || !handle || !out) return false;
                const auto t=reinterpret_cast<uintptr_t>(task);
                const auto h=reinterpret_cast<uintptr_t>(handle);
                out->task=t;
                out->ability=*reinterpret_cast<uintptr_t*>(t+0x68);
                if(out->ability)
                    out->abilityClass=*reinterpret_cast<uintptr_t*>(out->ability+0x10);
                out->count=*reinterpret_cast<int*>(h+0x20);
                if(out->count<0 || out->count>256) return false;
                if(out->count) {
                    uintptr_t storage=*reinterpret_cast<uintptr_t*>(h+0x18);
                    if(!storage) storage=h+0x08;
                    const auto first=*reinterpret_cast<uintptr_t*>(storage);
                    if(first) out->targetType=*reinterpret_cast<uintptr_t*>(first);
                }
                return true;
            } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        inline void* __fastcall Detour(void* task, void* output)
        {
            // Always call the game's original first, exactly once.
            void* result=original(task,output);
            if(!recording.load(std::memory_order_acquire)) return result;
            const auto now=GetTickCount64();
            if(now>=endMs.load(std::memory_order_relaxed)) {
                recording.store(false,std::memory_order_release);
                return result;
            }
            Sample s{}; s.ms=now-startMs.load(std::memory_order_relaxed);
            s.tid=GetCurrentThreadId();
            s.readError=ReadSample(task,output,&s)?0:1;
            if(mutex.try_lock()) {
                if(recording.load(std::memory_order_relaxed)) {
                    const bool baseline=s.ms<4000;
                    int& phaseCount=baseline?baselineCount:attackCount;
                    if(phaseCount<PerPhase)
                        samples[(baseline?0:PerPhase)+phaseCount++]=s;
                    else dropped.fetch_add(1,std::memory_order_relaxed);
                } else dropped.fetch_add(1,std::memory_order_relaxed);
                mutex.unlock();
            } else dropped.fetch_add(1,std::memory_order_relaxed);
            return result;
        }
        // This is a build-specific native function signature. Refuse to hook
        // unless the match is unique AND its RVA is the one identified offline.
        inline void* ResolveCandidate(int* matches)
        {
            __try {
                const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                if(!base) return nullptr;
                const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>0x100000) return nullptr;
                const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
                if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                   nt->FileHeader.NumberOfSections==0 || nt->FileHeader.NumberOfSections>96) return nullptr;
                const size_t imageSize=nt->OptionalHeader.SizeOfImage;
                const auto* sec=IMAGE_FIRST_SECTION(nt);
                static const unsigned char sig[]={
                    0x40,0x55,0x53,0x56,0x48,0x8d,0x6c,0x24,0xb9,
                    0x48,0x81,0xec,0xb0,0x00,0x00,0x00,0x48,0x8b,0x05,
                    0,0,0,0,0x48,0x33,0xc4,0x48,0x89,0x45,0x3f,0xc6,0x02,0x00,0x48,0x8b,0xda
                };
                uintptr_t found=0; int n=0;
                for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
                    if(!(sec[i].Characteristics & IMAGE_SCN_CNT_CODE) || !(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                    const size_t start=sec[i].VirtualAddress, len=sec[i].Misc.VirtualSize;
                    if(start>=imageSize || len>imageSize-start || len<sizeof(sig)) continue;
                    const auto* p=reinterpret_cast<const unsigned char*>(base+start);
                    for(size_t k=0;k<=len-sizeof(sig);++k) {
                        if(p[k]!=sig[0] || std::memcmp(p+k,sig,19)!=0 ||
                           std::memcmp(p+k+23,sig+23,sizeof(sig)-23)!=0) continue;
                        found=base+start+k; ++n;
                        if(n>1) { *matches=n; return nullptr; }
                    }
                }
                *matches=n;
                if(n!=1 || found-base!=0x05CDE450) return nullptr;
                imageBase=base;
                return reinterpret_cast<void*>(found);
            } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        }
        inline bool Install()
        {
            if(installed) return true;
            int matches=0; void* target=ResolveCandidate(&matches);
            if(!target) {status="Unavailable: unsupported game build or signature is not unique.";return false;}
            const auto created=MH_CreateHook(target,reinterpret_cast<void*>(&Detour),reinterpret_cast<void**>(&original));
            if(created!=MH_OK) { status="Hook creation failed (MinHook status "+std::to_string(static_cast<int>(created))+")."; return false; }
            const auto enabled=MH_EnableHook(target);
            if(enabled!=MH_OK) {MH_RemoveHook(target);original=nullptr;status="Hook enable failed (MinHook status "+std::to_string(static_cast<int>(enabled))+").";return false;}
            installed=true;return true;
        }
        inline std::string Report()
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::ostringstream out;
            out<<"Basic aim trace v1 | expected game SDK build 1641736\n"
               <<"Read-only data collection; inline hook installed until DLL unload. Not a hit or server trace.\n"
               <<"Samples baseline="<<baselineCount<<" attack_phase="<<attackCount<<" dropped="<<dropped.load()<<" recording="<<(recording.load()?"yes":"no")<<"\n"
               <<"t_ms tid task ability ability_class target_vtable_rva data_count read_error\n";
            for(int i=0;i<baselineCount+attackCount;++i) {
                const auto& s=samples[i<baselineCount?i:PerPhase+i-baselineCount];
                out<<s.ms<<' '<<s.tid<<" 0x"<<std::hex<<s.task<<" 0x"<<s.ability<<" 0x"<<s.abilityClass;
                if(s.targetType>=imageBase && s.targetType-imageBase<0x0C64C000ULL)
                    out<<" 0x"<<(s.targetType-imageBase);
                else out<<" unknown";
                out<<std::dec<<' '<<s.count<<' '<<s.readError<<'\n';
            }
            out<<"Times are from Start. Calls may belong to other actors, abilities, UI or prediction; sample count is not shot count.\n";
            return out.str();
        }
    }
    inline void DrawPanel()
    {
        using namespace Detail;
        UITheme::BeginCard("basic_aim_trace",UITheme::Icon::Info,"Basic aim trace - read only");
        UITheme::InfoText("One 15-second recording. Wait 4 seconds, then fire several basic attacks. Not silent aim. Hook remains until trainer unloads.");
        bool active=recording.load(std::memory_order_acquire);
        if(active && GetTickCount64()>=endMs.load()) {recording.store(false);active=false;status="Finished; copy the report.";}
        ImGui::BeginDisabled(active);
        if(ImGui::Button("Start aim trace (15 s)",{UITheme::Px(235.f),UITheme::Px(36.f)})) {
            if(Install()) {
                {std::lock_guard<std::mutex> lock(mutex); baselineCount=0;attackCount=0;}
                dropped.store(0);const auto now=GetTickCount64();startMs.store(now);endMs.store(now+15000);
                recording.store(true,std::memory_order_release);status="Recording: close menu, wait 4 seconds, then attack.";
            }
        }
        ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(!active);
        if(ImGui::Button("Stop aim trace")) {recording.store(false);status="Stopped; copy the report.";}
        ImGui::EndDisabled();
        ImGui::BeginDisabled(active || !installed);if(ImGui::Button("Copy aim trace")) {const auto r=Report();ImGui::SetClipboardText(r.c_str());}ImGui::EndDisabled();
        ImGui::TextWrapped("Status: %s",status.c_str());
        UITheme::InfoText("The report includes process memory addresses. Sharing it is optional. Anti-cheat may detect injected hooks.");
        UITheme::EndCard();
    }
}
