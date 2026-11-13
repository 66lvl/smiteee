#include "auto_laugh.h"
#include "auto_laugh_logic.h"
#include "../core/sdk_helpers.h"
#include "../core/hooks.h"
#include "../ui/theme.h"
#include <atomic>
#include <mutex>
#include <string>
#include <cstdio>

namespace AutoLaugh
{
    static std::mutex s_mutex,s_dispatch;
    static Config s_config;
    static std::string s_status="Disabled";
    static std::atomic_bool s_enabled{false},s_stopping{false},s_uiBlocked{true},s_rearm{true};
    static std::atomic<UINT> s_setup{0};
    static std::atomic<HWND> s_window{nullptr};
    static std::atomic<unsigned> s_calls{0};
    static std::atomic<uint64_t> s_lastTick{0};
    static uint64_t s_lastPost=0; // render thread only
    static int s_timerToken;
    static const UINT_PTR TimerId=reinterpret_cast<UINT_PTR>(&s_timerToken);
    // Window thread only; never retain game UObject pointers.
    static Logic::Gate s_gate;
    static uintptr_t s_world=0;
    Config GetConfig(){std::lock_guard<std::mutex> lock(s_mutex);return s_config;}
    void SetConfig(const Config& value)
    {
        Config next=value;next.IntervalSeconds=Logic::ClampInterval(next.IntervalSeconds);
        if(!Logic::AllowedKey(next.HoldKey))next.HoldKey=0;
        std::lock_guard<std::mutex> lock(s_mutex);s_config=next;s_enabled=next.Enabled;s_rearm=true;
    }
    static void Publish(const char* text){std::lock_guard<std::mutex> lock(s_mutex);s_status=text;}
    static bool Live(SDK::UObject* object)
    {
        if(!Helpers::IsValidPtr(object))return false;
        const auto bad=static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject)|static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
        return !(static_cast<uint32_t>(object->Flags)&bad);
    }
    // Uses the game's own loaded action table, not a guessed tag or a local emote.
    // A missing, mismatched or ambiguous table fails closed.
    static bool FindLaugh(SDK::AHWVGSManager* manager,SDK::FGameplayTag* result)
    {
        auto* table=manager->ActionTableSoftPtr.Get();
        if(!Live(table)||!Live(table->RowStruct)||table->RowStruct->GetName()!="HWVGSActionRow")return false;
        // This dumped SDK has a broken const TSparseArray::operator[] (casts
        // away const internally). Select its valid non-const accessor, but keep
        // the action row const: this code still does not modify the table.
        auto& rows=table->RowMap;const int n=rows.NumAllocated();
        if(!rows.IsValid()||n<=0||n>4096||rows.Num()<0||rows.Num()>n)return false;
        bool found=false;SDK::FName name{};
        for(int i=0;i<n;i++){
            if(!rows.IsValidIndex(i))continue;
            auto* row=reinterpret_cast<const SDK::FHWVGSActionRow*>(rows[i].Value());
            if(!Helpers::IsValidPtr(row)||!row->bEnabled)continue;
            const auto id=row->ID.TagName.ToString();
            if(id.empty()||id=="None")continue;
            if(!Logic::IsLaughTag(id)&&!Logic::IsLaughTag(row->OutputEmote.TagName.ToString()))continue;
            if(found&&row->ID.TagName!=name)return false;
            *result=row->ID;name=row->ID.TagName;found=true;
        }
        return found;
    }
    static bool GameReady(SDK::AHWPlayerController_Smite** output,SDK::AHWVGSManager** vgs,SDK::UWorld** outWorld)
    {
        auto* world=Helpers::GetWorld();auto* raw=Helpers::GetLocalPC();
        if(!Live(world)||!Live(raw)||!raw->IsA(SDK::AHWPlayerController_Smite::StaticClass()))return false;
        auto* pc=static_cast<SDK::AHWPlayerController_Smite*>(raw);
        if(pc->bIsDeathSpectating||pc->bShowMouseCursor||!Live(pc->PlayerState)||pc->PlayerState->bIsInactive||pc->PlayerState->bFromPreviousLevel)return false;
        auto* pawn=pc->Pawn;
        if(!Live(pawn)||!pawn->IsA(SDK::AHWCharacter_PlayableCharacter::StaticClass()))return false;
        auto* character=static_cast<SDK::AHWCharacter_PlayableCharacter*>(pawn);
        if(character->IsDead()||!std::isfinite(character->GetHealth())||character->GetHealth()<=0.0)return false;
        if(SDK::UGameplayStatics::IsGamePaused(world))return false;
        auto* hud=pc->GetHUD();
        if(!Live(hud)||!hud->IsA(SDK::AHWGameHUD::StaticClass()))return false;
        // HemingwayUI_functions.cpp is not part of the original project. Use its
        // reflected getter with the SDK parameter layout, without editing flags.
        auto* modeFn=hud->Class->GetFunction("HWBaseHUD","GetActiveInputMode");
        if(!Live(modeFn))return false;
        struct ModeResult {SDK::ECommonInputMode ReturnValue=SDK::ECommonInputMode::Menu;} mode;
        hud->ProcessEvent(modeFn,&mode);
        if(mode.ReturnValue!=SDK::ECommonInputMode::Game)return false;
        const auto& stack=static_cast<SDK::AHWGameHUD*>(hud)->InputHandlerWidgetsStack;
        if(stack.Num()<0||stack.Num()>64||stack.Max()<stack.Num())return false;
        for(int i=0;i<stack.Num();i++){
            if(!stack.IsValidIndex(i)||!Live(stack[i]))return false;
            if(stack[i]->IsA(SDK::UHWChatWindowWidget::StaticClass())&&
               static_cast<SDK::UHWChatWindowWidget*>(stack[i])->CachedWindowState==SDK::EHWChatWindowState::Full)return false;
        }
        if(!Live(world->GameState)||!world->GameState->IsA(SDK::AHWGameState::StaticClass()))return false;
        auto* manager=static_cast<SDK::AHWGameState*>(world->GameState)->GetVGSManager();
        if(!Live(manager))return false;
        *output=pc;*vgs=manager;*outWorld=world;return true;
    }
    static void Poll(HWND hwnd)
    {
        const auto config=GetConfig();const auto now=GetTickCount64();
        if(s_rearm.exchange(false))s_gate.NeedRelease=true;
        const bool held=Logic::AllowedKey(config.HoldKey)&&(GetAsyncKeyState(config.HoldKey)&0x8000)!=0;
        if(!config.Enabled||s_uiBlocked.load()||GetForegroundWindow()!=hwnd){s_gate.Ready(now,held,false,config.IntervalSeconds);Publish(config.Enabled?"Paused: menu / text input / game unfocused":"Disabled");return;}
        if(!held){s_gate.Ready(now,false,true,config.IntervalSeconds);Publish(config.HoldKey?"Ready: hold the assigned key":"Assign a hold key first");return;}
        SDK::AHWPlayerController_Smite* pc=nullptr;SDK::AHWVGSManager* manager=nullptr;SDK::UWorld* world=nullptr;
        if(!GameReady(&pc,&manager,&world)){s_gate.Ready(now,true,false,config.IntervalSeconds);Publish("Paused: shop / chat / menu / dead / game unavailable");return;}
        if(s_world&&s_world!=reinterpret_cast<uintptr_t>(world))s_gate.NeedRelease=true;
        s_world=reinterpret_cast<uintptr_t>(world);
        if(s_gate.NeedRelease){Publish("Release the key, then hold again");return;}
        if(!s_gate.Ready(now,true,true,config.IntervalSeconds))return;
        SDK::FGameplayTag tag{};
        if(!FindLaugh(manager,&tag)){Publish("No unique enabled Laugh action in the loaded VGS table");return;}
        auto* fn=pc->Class->GetFunction("HWPlayerController","HandleInvokeVGSAction");
        if(!Live(fn)){Publish("Native VGS action handler unavailable");return;}
        // Same local action handler invoked by the VGS UI. Never edit flood timers,
        // item ownership or cooldowns; no raw network / multicast calls.
        pc->HandleInvokeVGSAction(tag);
        Publish("Laugh requested while held; game cooldown / flood protection still applies");
    }
    static void PollSafe(HWND hwnd)
    {
        __try {Poll(hwnd);}
        __except(EXCEPTION_EXECUTE_HANDLER){s_gate.NeedRelease=true;Publish("VGS access failed; release key to retry");}
    }
    void Service(bool menuOrTextInput)
    {
        s_uiBlocked=menuOrTextInput;
        if(s_stopping.load()||!s_enabled.load())return;
        HWND hwnd=Hooks::g_hWnd;if(!hwnd||!IsWindow(hwnd))return;
        const auto now=GetTickCount64();
        if(s_window.load()==hwnd&&now-s_lastTick.load()<2000)return;
        if(now-s_lastPost<1000)return;s_lastPost=now;
        if(!s_setup.load())s_setup=RegisterWindowMessageW(L"ALWAYSLOSE.AutoLaugh.Hold.v1");
        if(s_setup.load())PostMessageW(hwnd,s_setup.load(),0,0);
    }
    bool HandleWindowMessage(HWND hwnd,UINT msg,WPARAM wp,LPARAM)
    {
        const UINT setup=s_setup.load();const bool isSetup=setup&&msg==setup;
        if(!isSetup&&!(msg==WM_TIMER&&wp==TimerId))return false;
        static thread_local bool handling=false;if(handling)return true;handling=true;
        ++s_calls;
        {std::lock_guard<std::mutex> lock(s_dispatch);
            if(!s_stopping.load()&&GetWindowThreadProcessId(hwnd,nullptr)==GetCurrentThreadId()){
                if(!s_enabled.load()){KillTimer(hwnd,TimerId);s_window=nullptr;Publish("Disabled");}
                else if(isSetup){HWND old=s_window.load();if(old&&old!=hwnd)KillTimer(old,TimerId);if(SetTimer(hwnd,TimerId,25,nullptr))s_window=hwnd;else Publish("Window timer unavailable");}
                else {static thread_local bool polling=false;if(!polling){polling=true;PollSafe(hwnd);s_lastTick=GetTickCount64();polling=false;}}
            }
        }
        --s_calls;handling=false;return true; // Never consumes a keyboard or mouse message.
    }
    void Shutdown()
    {
        s_stopping=true;
        {std::lock_guard<std::mutex> lock(s_dispatch);HWND hwnd=s_window.exchange(nullptr);if(hwnd)KillTimer(hwnd,TimerId);}
        while(s_calls.load())Sleep(1);
    }
    void DrawSettings()
    {
        Config config=GetConfig();bool changed=false;
        UITheme::BeginCard("auto_laugh",UITheme::Icon::Spark,"Auto Laugh - hold key");
        changed|=UITheme::CheckboxRow("Enable Auto Laugh",&config.Enabled);
        UITheme::HelpLastItem("Auto Laugh","Repeats the native Laugh action only while held. Pauses in shop, chat, menus and while dead. Game limits still apply.");
        static bool capturing=false,armed=false;static double captureUntil=0.0;
        char name[48]="Unbound";
        if(config.HoldKey){if(config.HoldKey==VK_XBUTTON1)snprintf(name,sizeof(name),"M4");else if(config.HoldKey==VK_XBUTTON2)snprintf(name,sizeof(name),"M5");else GetKeyNameTextA(MapVirtualKeyA(config.HoldKey,MAPVK_VK_TO_VSC)<<16,name,sizeof(name));}
        ImGui::TextUnformatted("Hold key");
        if(ImGui::Button(capturing?"Press key...##laugh_key":name,{UITheme::Px(160.f),UITheme::Px(34.f)})){capturing=true;armed=false;captureUntil=ImGui::GetTime()+10.0;}
        if(capturing){
            bool any=false;for(int k=1;k<256;k++)if(GetAsyncKeyState(k)&0x8000)any=true;
            if(!any)armed=true;
            if(ImGui::GetTime()>captureUntil)capturing=false;
            if(capturing&&armed)for(int k=1;k<256;k++)if(GetAsyncKeyState(k)&0x8000){
                if(k==VK_ESCAPE){capturing=false;break;}
                if(k==VK_BACK){config.HoldKey=0;changed=true;capturing=false;break;}
                if(Logic::AllowedKey(k)){config.HoldKey=k;changed=true;capturing=false;break;}
            }
            UITheme::InfoText("Release the click, then press a key. Escape cancels; Backspace clears. Primary mouse, chat and menu keys are reserved.");
        }
        changed|=UITheme::SliderFloatRow("Laugh interval",&config.IntervalSeconds,.025f,10.f,"%.3f s");
        if(ImGui::Button("Use 0.025 s")){config.IntervalSeconds=.025f;changed=true;}
        if(changed)SetConfig(config);
        UITheme::InfoText("Rapid mode: down to 0.025 s (at most 40 requests/second). The game may reject repeats or keep the current audio playing.");
        UITheme::InfoText("Hold only - no toggle. Release to stop requesting new laughs. An animation already started can finish normally.");
        UITheme::InfoText("After opening UI, death or a transition, release and press the key again. No cooldown / flood-protection bypass.");
        {std::lock_guard<std::mutex> lock(s_mutex);ImGui::TextWrapped("Status: %s",s_status.c_str());}
        UITheme::EndCard();
    }
}
