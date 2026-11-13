#pragma once
// User-held, bounded right-click experiment, serviced by Menu::Draw.
// It does not alter UE memory, bypass validation or confirm the bug occurred.
#include "nox_quick_exit_logic.h"
#include "../core/sdk_helpers.h"
#include "../core/hooks.h"
#include "../ui/theme.h"
#include <Windows.h>

namespace NoxQuickExit
{
    namespace
    {
        bool s_enabled=false,s_lastThree=false;
        float s_intervalMs=25.f;
        const char* s_status="Disabled";
        NoxQuickExitLogic::Gate s_gate;
        SDK::UWorld* s_world=nullptr;
        SDK::APawn* s_pawn=nullptr;
        bool Live(SDK::UObject* o)
        {
            if(!Helpers::IsValidPtr(o)||!Helpers::IsValidPtr(o->Class))return false;
            const auto bad=static_cast<unsigned>(SDK::EObjectFlags::ClassDefaultObject)|
                static_cast<unsigned>(SDK::EObjectFlags::BeginDestroyed)|
                static_cast<unsigned>(SDK::EObjectFlags::FinishDestroyed)|
                static_cast<unsigned>(SDK::EObjectFlags::MirroredGarbage);
            return !(static_cast<unsigned>(o->Flags)&bad);
        }
        bool Context(SDK::UWorld** outWorld,SDK::APawn** outPawn)
        {
            auto* world=Helpers::GetWorld();auto* raw=Helpers::GetLocalPC();
            if(!Live(world)||!Live(world->GameState)||!world->GameState->IsA(SDK::AHWGameState::StaticClass()))return false;
            if(!Live(raw)||!raw->IsA(SDK::AHWPlayerController_Smite::StaticClass()))return false;
            auto* pc=static_cast<SDK::AHWPlayerController_Smite*>(raw);
            if(pc->bIsDeathSpectating||pc->bShowMouseCursor||!Live(pc->PlayerState)||
                pc->PlayerState->bIsInactive||pc->PlayerState->bFromPreviousLevel)return false;
            auto* pawn=pc->Pawn;
            if(!Live(pawn)||!pawn->IsA(SDK::AHWCharacter_Nox::StaticClass())||pawn->Controller!=pc)return false;
            auto* character=static_cast<SDK::AHWCharacter_Nox*>(pawn);
            if(character->IsDead()||!std::isfinite(character->GetHealth())||character->GetHealth()<=0.0)return false;
            if(SDK::UGameplayStatics::IsGamePaused(world))return false;
            auto* hud=pc->GetHUD();
            if(!Live(hud)||!hud->IsA(SDK::AHWGameHUD::StaticClass()))return false;
            auto* fn=hud->Class->GetFunction("HWBaseHUD","GetActiveInputMode");
            if(!Live(fn))return false;
            struct ModeResult {SDK::ECommonInputMode ReturnValue=SDK::ECommonInputMode::Menu;} mode;
            hud->ProcessEvent(fn,&mode);
            if(mode.ReturnValue!=SDK::ECommonInputMode::Game)return false;
            const auto& stack=static_cast<SDK::AHWGameHUD*>(hud)->InputHandlerWidgetsStack;
            if(stack.Num()<0||stack.Num()>64||stack.Max()<stack.Num())return false;
            for(int i=0;i<stack.Num();++i){
                if(!stack.IsValidIndex(i)||!Live(stack[i]))return false;
                if(stack[i]->IsA(SDK::UHWChatWindowWidget::StaticClass())&&
                   static_cast<SDK::UHWChatWindowWidget*>(stack[i])->CachedWindowState==SDK::EHWChatWindowState::Full)return false;
            }
            *outWorld=world;*outPawn=pawn;return true;
        }
        bool ContextSafe(SDK::UWorld** world,SDK::APawn** pawn)
        {__try{return Context(world,pawn);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
        void Cancel(const char* message){s_gate.Reset();s_world=nullptr;s_pawn=nullptr;s_status=message;}
        void Tick(bool blocked)
        {
            const bool f8=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
            const bool three=(GetAsyncKeyState('3')&0x8000)!=0;
            const bool thirdEdge=three&&!s_lastThree;
            s_lastThree=three;
            if(!s_enabled){if(s_gate.running)Cancel("Disabled");s_gate.Reset();return;}
            HWND hwnd=Hooks::g_hWnd;
            if(blocked||!hwnd||!IsWindow(hwnd)||GetForegroundWindow()!=hwnd)
            {if(s_gate.running)Cancel("Stopped: menu, text input or game unfocused.");else s_gate.Reset();return;}
            // Require a full release after enabling, stopping or leaving the game.
            if(s_gate.requireRelease){
                s_gate.Step(GetTickCount64(),true,f8,thirdEdge,!f8&&!three,25);
                s_status=s_gate.requireRelease?"Release F8 and 3 to arm.":"Ready: hold F8 and press 3.";
                return;
            }
            if(!s_gate.running&&!(f8&&thirdEdge))return;
            SDK::UWorld* world=nullptr;SDK::APawn* pawn=nullptr;
            if(!ContextSafe(&world,&pawn)){Cancel("Stopped: Nox gameplay context unavailable (shop/chat/dead/etc).");return;}
            if(s_gate.running&&(world!=s_world||pawn!=s_pawn))
            {Cancel("Stopped: world or character changed.");return;}
            if(!s_gate.running){s_world=world;s_pawn=pawn;}
            const unsigned interval=static_cast<unsigned>(s_intervalMs<25.f?25.f:(s_intervalMs>250.f?250.f:s_intervalMs));
            const auto event=s_gate.Step(GetTickCount64(),true,f8,thirdEdge,!f8&&!three,interval);
            if(event==NoxQuickExitLogic::Event::Stopped){Cancel("Stopped: F8 released.");return;}
            if(event==NoxQuickExitLogic::Event::Expired){Cancel("Stopped after 2 seconds / 80 clicks; release F8 and 3.");return;}
            if(event!=NoxQuickExitLogic::Event::ClickDue)return;
            // No catch-up bursts: at most one click per rendered frame.
            if(GetForegroundWindow()!=hwnd||(GetAsyncKeyState(VK_RBUTTON)&0x8000))
            {Cancel("Stopped: focus lost or physical right button held.");return;}
            INPUT events[2]{};events[0].type=INPUT_MOUSE;events[0].mi.dwFlags=MOUSEEVENTF_RIGHTDOWN;
            events[1].type=INPUT_MOUSE;events[1].mi.dwFlags=MOUSEEVENTF_RIGHTUP;
            const UINT sent=SendInput(2,events,sizeof(INPUT));
            if(sent==1){INPUT release{};release.type=INPUT_MOUSE;release.mi.dwFlags=MOUSEEVENTF_RIGHTUP;SendInput(1,&release,sizeof(INPUT));}
            if(sent!=2){Cancel("Stopped: right-click injection failed or was partial.");return;}
            s_status="Repeating right click while F8 held; entry / bug success not verified.";
        }

    }
    inline void Service(bool blocked){Tick(blocked);}
    inline void DrawPanel()
    {
        UITheme::BeginCard("nox_quick_exit",UITheme::Icon::Combat,"Nox quick exit - experiment");
        if(UITheme::CheckboxRow("Enable Nox repeated exit",&s_enabled)&&!s_enabled)Cancel("Disabled");
        UITheme::SliderFloatRow("Right-click interval (ms)",&s_intervalMs,25.f,250.f,"%.0f ms");
        ImGui::TextWrapped("Status: %s",s_status);
        UITheme::InfoText("Close menu, release F8 and 3, then hold F8 and press 3 normally. Repeats right click until F8 is released, up to 2 seconds / 80 clicks.");
        UITheme::InfoText("Requires Nox, game focus and 3 bound to ability 3. Clicks begin when 3 is pressed, not when entry is confirmed. Timing is limited by rendering frame rate; the game may ignore synthetic input.");
        UITheme::InfoText("Disabled by default and not saved. Menu, focus loss, death, shop/chat or character change stops the sequence. The bug is not verified. Test only in a permitted practice environment.");
        UITheme::EndCard();
    }
}
