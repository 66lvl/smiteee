#include "status_overlay.h"
#include "crosshair.h"
#include "../ui/menu.h"
#include "../core/game_state.h"
#include "../core/sdk_helpers.h"
#include "../core/hooks.h"
#include "../ui/theme.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <string>

namespace StatusOverlay
{
    // This is a best-effort, client-visible list, not a server spectator subscription.
    // Remote PlayerControllers generally are NOT replicated to an ordinary UE client.
    // Missing controllers/targets are UNKNOWN, never inferred from health/death alone.
    enum class Coverage { Waiting, Unavailable, Partial, Complete };
    struct Snapshot
    {
        int ping=-1,count=0;
        Coverage coverage=Coverage::Waiting;
        char names[16][192]{};
        uint64_t sampledAt=0;
    };
    static Config s_config;
    static Snapshot s_snapshot,s_renderSnapshot;
    static std::mutex s_mutex;
    static std::atomic_bool s_needPing{false},s_needSpectators{false},s_stopping{false};
    static std::atomic<HWND> s_window{nullptr};
    static std::atomic<UINT> s_setup{0};
    static std::atomic<uint64_t> s_lastTick{0};
    static std::atomic_uint s_calls{0};
    static uint64_t s_lastPost=0; // Present thread only
    static int s_timerToken=0;
    static const UINT_PTR TimerId=reinterpret_cast<UINT_PTR>(&s_timerToken);

    Config GetConfig(){std::lock_guard<std::mutex> lock(s_mutex);return s_config;}
    void SetConfig(const Config& options)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_config=options;s_snapshot={};
        s_needPing=options.ShowPing;s_needSpectators=options.ShowSpectators;
    }
    static bool Live(SDK::UObject* obj)
    {
        if(!Helpers::IsValidPtr(obj))return false;
        const auto flags=static_cast<uint32_t>(obj->Flags);
        const uint32_t bad=static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject)|
            static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed)|
            static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed)|
            static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
        return (flags&bad)==0;
    }
    // POD-only reads are isolated behind SEH, keeping C++ owning objects outside __try.
    static bool ReadContext(SDK::UWorld** world,SDK::AHWPlayerController** pc,SDK::APlayerState** ps,int* ping)
    {
        __try {
            auto* w=Helpers::GetWorld();auto* p=Helpers::GetLocalPC();
            if(!Live(w)||!Live(p)||!p->IsA(SDK::AHWPlayerController::StaticClass()))return false;
            auto* state=p->PlayerState;
            if(!Live(state)||state->bIsInactive||state->bFromPreviousLevel)return false;
            *world=w;*pc=static_cast<SDK::AHWPlayerController*>(p);*ps=state;
            // UE's replicated byte is quantized to four milliseconds, not an ICMP probe.
            *ping=w->NetDriver && state->CompressedPing>0 ? static_cast<int>(state->CompressedPing)*4 : -1;
            return true;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static int ReadPlayers(SDK::UWorld* world,SDK::APlayerState** players,int capacity)
    {
        __try {
            if(!Live(world)||!Live(world->GameState))return -1;
            auto& list=world->GameState->PlayerArray;
            const int n=list.Num();if(n<0||n>capacity||list.Max()<n)return -1;
            for(int i=0;i<n;++i){if(!list.IsValidIndex(i))return -1;players[i]=list[i];}
            return n;
        }__except(EXCEPTION_EXECUTE_HANDLER){return -1;}
    }
    // Return -1 unknown, 0 excluded/not watching, 1 confirmed ally watching local PS.
    static int ReadObserver(SDK::APlayerState* ps,SDK::AHWPlayerController* local,
                            SDK::APlayerState* localPS,wchar_t* name,int nameCapacity)
    {
        __try {
            if(!Live(ps))return -1;
            if(ps==localPS||ps->bIsABot||ps->bIsInactive||ps->bFromPreviousLevel)return 0;
            auto* owner=ps->Owner;
            if(!Live(owner)||!owner->IsA(SDK::AHWPlayerController::StaticClass()))return -1;
            auto* other=static_cast<SDK::AHWPlayerController*>(owner);
            if(other==local||other->PlayerState!=ps)return -1;
            auto* mine=local->CurrentABSActorTeamState.Get();
            auto* theirs=other->CurrentABSActorTeamState.Get();
            if(!Live(mine)||!Live(theirs))return -1;
            if(mine!=theirs)return 0;
            auto* module=other->ControlModule;
            if(!Live(module))return -1;
            if(!module->IsA(SDK::UHWControlModule_DeathSpectator::StaticClass()))return 0;
            auto* death=static_cast<SDK::UHWControlModule_DeathSpectator*>(module);
            auto* target=death->SpectatedPlayerState.Get();
            if(!Live(target))return -1; // Do NOT guess using the selection index or a dead flag.
            if(target!=localPS)return 0;
            if(!Live(localPS->PawnPrivate))return -1; // No active local character to observe.
            const auto& text=ps->PlayerNamePrivate;
            const int len=text.Num();
            if(len<=0||len>1024||text.Max()<len||!Helpers::IsValidPtr(text.GetDataPtr()))return -1;
            int i=0;
            for(;i<len&&i<nameCapacity-1;i++){
                const wchar_t c=text.GetDataPtr()[i];if(!c)break;
                name[i]=(c<32||c==127)?L' ':c;
            }
            name[i]=0;return i?1:-1;
        }__except(EXCEPTION_EXECUTE_HANDLER){return -1;}
    }
    static void Poll()
    {
        Snapshot next;next.sampledAt=GetTickCount64();next.coverage=Coverage::Unavailable;
        SDK::UWorld* world=nullptr;SDK::AHWPlayerController* pc=nullptr;SDK::APlayerState* localPS=nullptr;
        int ping=-1;
        if(ReadContext(&world,&pc,&localPS,&ping)){
            if(s_needPing.load())next.ping=ping;
            if(s_needSpectators.load()){
                SDK::APlayerState* players[64]{};const int n=ReadPlayers(world,players,64);int known=0,unknown=0;
                if(n>0){
                    for(int i=0;i<n;i++){
                        if(players[i]==localPS)continue;
                        bool duplicate=false;for(int j=0;j<i;j++)if(players[j]==players[i])duplicate=true;
                        if(duplicate)continue;
                        wchar_t name[64]{};int result=ReadObserver(players[i],pc,localPS,name,64);
                        if(result<0){++unknown;continue;}++known;
                        if(result==1&&next.count<16){
                            int written=WideCharToMultiByte(CP_UTF8,0,name,-1,next.names[next.count],192,nullptr,nullptr);
                            if(written>1)++next.count;else ++unknown;
                        }else if(result==1)++unknown;
                    }
                    next.coverage=unknown?(known?Coverage::Partial:Coverage::Unavailable):Coverage::Complete;
                }
            }
        }
        // A world/player switch during this bounded pass invalidates the entire sample.
        SDK::UWorld* endWorld=nullptr;SDK::AHWPlayerController* endPC=nullptr;SDK::APlayerState* endPS=nullptr;int ignored=-1;
        if(!ReadContext(&endWorld,&endPC,&endPS,&ignored)||endWorld!=world||endPC!=pc||endPS!=localPS){
            next.ping=-1;next.count=0;next.coverage=Coverage::Unavailable;
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        if(!s_config.ShowPing)next.ping=-1;
        if(!s_config.ShowSpectators){next.count=0;next.coverage=Coverage::Waiting;}
        s_snapshot=next;
    }
    struct ActiveCall{ActiveCall(){++s_calls;}~ActiveCall(){--s_calls;}};
    bool HandleWindowMessage(HWND hwnd,UINT msg,WPARAM wp,LPARAM)
    {
        const UINT setup=s_setup.load();const bool isSetup=setup&&msg==setup;
        if(!isSetup&&!(msg==WM_TIMER&&wp==TimerId))return false;
        ActiveCall active;
        if(s_stopping.load())return true;
        if(!hwnd||GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId())return true;
        if(!s_needPing.load()&&!s_needSpectators.load()){KillTimer(hwnd,TimerId);s_window=nullptr;return true;}
        if(isSetup){
            HWND old=s_window.load();if(old&&old!=hwnd)KillTimer(old,TimerId);
            if(SetTimer(hwnd,TimerId,500,nullptr))s_window=hwnd;
            return true;
        }
        // Bounded, read-only sample on the window dispatcher. No ProcessEvent, RPC, asset loads,
        // global actor enumeration or background SDK thread. HWND owner != proven UE game thread.
        static thread_local bool polling=false;if(polling)return true;
        polling=true;Poll();s_lastTick=GetTickCount64();polling=false;return true;
    }
    void Service()
    {
        if(s_stopping.load()||(!s_needPing.load()&&!s_needSpectators.load()))return;
        HWND hwnd=Hooks::g_hWnd;if(!hwnd||!IsWindow(hwnd))return;
        const uint64_t now=GetTickCount64();
        if(s_window.load()==hwnd&&now-s_lastTick.load()<2000)return;
        if(now-s_lastPost<1000)return;s_lastPost=now;
        if(!s_setup.load())s_setup=RegisterWindowMessageW(L"ALWAYSLOSE.StatusOverlay.v1");
        const UINT setup=s_setup.load();if(setup)PostMessageW(hwnd,setup,0,0);
    }
    static const char* CoverageText(Coverage c)
    {
        switch(c){case Coverage::Complete:return "No allies detected watching you";
        case Coverage::Partial:return "Partial data - list may be incomplete";
        case Coverage::Unavailable:return "Observer targets unavailable on this client";
        default:return "Waiting for spectator data";}
    }
    static void Panel(ImVec2 p,ImVec2 size)
    {
        auto* dl=ImGui::GetBackgroundDrawList();
        dl->AddRectFilled(p,{p.x+size.x,p.y+size.y},UITheme::ToU32(UITheme::palette.glassDeep),10.f);
        dl->AddRect(p,{p.x+size.x,p.y+size.y},UITheme::ToU32(UITheme::palette.border),10.f);
    }
    void Draw()
    {
        // Must run before the optional FPS/ping/spectator early return.
        Crosshair::Draw(Menu::IsOpen,Hooks::g_hWnd&&GetForegroundWindow()==Hooks::g_hWnd,GameState::IsPlayable());
        Config options;
        {std::unique_lock<std::mutex> lock(s_mutex,std::try_to_lock);if(!lock.owns_lock())return;options=s_config;
            if(!options.ShowFps&&!options.ShowPing&&!options.ShowSpectators)return;
            if(options.ShowPing||options.ShowSpectators)s_renderSnapshot=s_snapshot;
        }
        Snapshot snap=s_renderSnapshot;
        if(!snap.sampledAt||GetTickCount64()-snap.sampledAt>2000){snap.ping=-1;snap.count=0;snap.coverage=Coverage::Waiting;}
        auto* dl=ImGui::GetBackgroundDrawList();float y=18.f;
        if(options.ShowFps||options.ShowPing){
            char line[96]{},fps[32]{},ping[32]{};
            if(options.ShowFps)snprintf(fps,sizeof(fps),"%.0f FPS",ImGui::GetIO().Framerate);
            if(options.ShowPing){if(snap.ping>=0)snprintf(ping,sizeof(ping),"~%d ms",snap.ping);else snprintf(ping,sizeof(ping),"Ping: N/A");}
            snprintf(line,sizeof(line),"%s%s%s",fps,options.ShowFps&&options.ShowPing?"   |   ":"",ping);
            float width=ImGui::CalcTextSize(line).x+26.f;Panel({18.f,y},{width,36.f});
            dl->AddText({31.f,y+10.f},UITheme::ToU32(UITheme::palette.text),line);y+=46.f;
        }
        if(options.ShowSpectators){
            const float w=320.f;const bool incomplete=snap.coverage!=Coverage::Complete;
            const float h=54.f+static_cast<float>(snap.count)*22.f+(incomplete||!snap.count?22.f:0.f);
            Panel({18.f,y},{w,h});char title[64];if(snap.coverage==Coverage::Unavailable||snap.coverage==Coverage::Waiting)snprintf(title,sizeof(title),"Spectators: N/A (not zero)");
            else snprintf(title,sizeof(title),"Spectators - confirmed: %d",snap.count);
            dl->AddText({31.f,y+12.f},UITheme::ToU32(UITheme::palette.accent),title);
            float row=y+40.f;
            for(int i=0;i<snap.count;i++){
                // Clip long names, without splitting their UTF-8 source string.
                dl->PushClipRect({31.f,row},{18.f+w-12.f,row+21.f},true);
                dl->AddText({31.f,row},UITheme::ToU32(UITheme::palette.text),snap.names[i]);dl->PopClipRect();row+=22.f;
            }
            if(incomplete||!snap.count){
                if(UITheme::fontSmall)ImGui::PushFont(UITheme::fontSmall);
                dl->AddText({31.f,row},UITheme::ToU32(UITheme::palette.textDim),CoverageText(snap.coverage));
                if(UITheme::fontSmall)ImGui::PopFont();
            }
        }
    }
    void DrawSettings()
    {
        Crosshair::DrawSettings();
        Config options=GetConfig();
        UITheme::BeginCard("hud_indicators",UITheme::Icon::Info,"On-screen indicators");
        bool changed=UITheme::CheckboxRow("Show FPS",&options.ShowFps);
        changed|=UITheme::CheckboxRow("Show Ping",&options.ShowPing);
        changed|=UITheme::CheckboxRow("Spectator list",&options.ShowSpectators);
        if(changed)SetConfig(options);
        UITheme::InfoText("FPS is the overlay frame rate. Ping is the replicated PlayerState value (~4 ms resolution); N/A when unavailable or zero/uninitialized.");
        ImGui::Spacing();UITheme::InfoText("Spectators: confirmed allies targeting your current player only. Remote controllers may not be replicated; unavailable is NOT zero spectators.");
        Snapshot diagnostic;{std::lock_guard<std::mutex> lock(s_mutex);diagnostic=s_snapshot;}
        if(options.ShowSpectators)ImGui::TextWrapped("Observer data: %s",CoverageText(diagnostic.coverage));
        UITheme::EndCard();
    }
    void Shutdown()
    {
        s_stopping=true;HWND hwnd=s_window.exchange(nullptr);if(hwnd)KillTimer(hwnd,TimerId);
        while(s_calls.load())Sleep(1);
        std::lock_guard<std::mutex> lock(s_mutex);s_snapshot={};
    }
}
