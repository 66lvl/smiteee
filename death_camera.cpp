#include "death_camera.h"
#include "../core/sdk_helpers.h"
#include "../core/hooks.h"
#include "../ui/theme.h"
#include "../ui/menu.h"
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace DeathCamera
{
    using Exec=SDK::UFunction::FNativeFuncPtr;
    struct Identity { uintptr_t address=0;int index=-1;SDK::FName name{}; };
    struct Context { SDK::UWorld* world=nullptr;SDK::AHWPlayerController_Smite* pc=nullptr;SDK::APlayerState* ps=nullptr;SDK::UHWControlModule_DeathSpectator* module=nullptr;bool dead=false; };
    struct Entry { SDK::AHWCharacter_PlayableCharacter* pawn=nullptr;SDK::APlayerState* ps=nullptr; };
    static std::atomic_bool s_enabled{false},s_active{false},s_stopping{false},s_suspend{false},s_hookReady{false},s_deadReady{false},s_leftCaptured{false},s_capture{false},s_lookPending{false},s_nativeLookReady{false};
    static std::atomic<int> s_pendingClicks{0};
    static unsigned s_samples=0;
    static std::atomic<DWORD> s_gameThread{0};
    static std::atomic<uintptr_t> s_observedPC{0};
    static std::atomic<int> s_command{0}; // -1 previous, +1 next, 2 restore
    static std::atomic_uint s_calls{0};
    static std::atomic<UINT> s_setup{0};
    static std::atomic<HWND> s_window{nullptr};
    static std::atomic<uint64_t> s_lastTick{0};
    static std::mutex s_mutex,s_dispatch;
    static std::string s_status="Disabled",s_target;
    static int s_candidates=0;
    static Exec s_original=nullptr;
    static void* s_hookTarget=nullptr;
    static int s_timerToken;
    static const UINT_PTR TimerId=reinterpret_cast<UINT_PTR>(&s_timerToken);
    static uint64_t s_lastPost=0,s_nextRoster=0;
    // The following identities belong exclusively to the verified window/game thread.
    // Numeric identities do not root UObjects. Resolve only from the CURRENT bounded roster.
    static Identity s_world,s_pc,s_saved,s_chosen;
    static Entry s_entries[64]{}; // scratch only: cleared after every pass, never kept across ticks
    static int s_count=0;
    struct LookLease {Identity world,pawn,arm;bool oldLocal=false,oldPawn=false,localWritten=false,pawnWritten=false;};
    static LookLease s_look;
    static int s_profileMode=-1;
    static bool ReleaseLookLease();

    Config GetConfig(){return {s_enabled.load()};}
    void SetConfig(const Config& c){s_enabled=c.Enabled;if(!c.Enabled){s_deadReady=false;s_command=2;}}
    static void Publish(const char* message,const char* target=nullptr,int count=-1)
    {
        std::lock_guard<std::mutex> lock(s_mutex);s_status=message;
        if(target)s_target=target;if(count>=0)s_candidates=count;
    }
    static bool Live(SDK::UObject* p)
    {
        if(!Helpers::IsValidPtr(p))return false;
        const uint32_t bad=static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject)|static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
        return !(static_cast<uint32_t>(p->Flags)&bad);
    }
    static Identity Id(SDK::UObject* p){return {reinterpret_cast<uintptr_t>(p),p->Index,p->Name};}
    static bool Same(const Identity& a,const Identity& b){return a.address==b.address&&a.index==b.index&&!(a.name!=b.name);}
    static bool Matches(const Identity& id,SDK::UObject* p){return p&&Same(id,Id(p));}
    static void Forget(const char* reason)
    {
        if(!ReleaseLookLease()){s_active=true;Publish("Camera flag restore pending; unload blocked");return;}
        s_saved={};s_chosen={};s_active=false;s_deadReady=false;Publish(reason,"",0);
    }
    static void ClearScratch(){for(auto& e:s_entries)e={};s_count=0;}
    static bool ReadContext(Context* c)
    {
        __try {
            auto* w=Helpers::GetWorld();auto* p=Helpers::GetLocalPC();
            if(!Live(w)||!Live(p)||!p->IsA(SDK::AHWPlayerController_Smite::StaticClass()))return false;
            auto* pc=static_cast<SDK::AHWPlayerController_Smite*>(p);auto* ps=pc->PlayerState;
            if(!Live(ps)||ps->bIsInactive||ps->bFromPreviousLevel||!Live(pc->PlayerCameraManager))return false;
            c->world=w;c->pc=pc;c->ps=ps;c->dead=pc->bIsDeathSpectating;
            auto* module=pc->ControlModule;
            if(Live(module)&&module->IsA(SDK::UHWControlModule_DeathSpectator::StaticClass()))c->module=static_cast<SDK::UHWControlModule_DeathSpectator*>(module);
            return true;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static bool GameUiOwnsMouse(const Context& c)
    {
        // The shop / pause UI exposes the game cursor. Never override that UI's input.
        // POD-only SEH helper: no UObject state is modified here.
        __try {return !Live(c.pc)||c.pc->bShowMouseCursor;}
        __except(EXCEPTION_EXECUTE_HANDLER){return true;}
    }
    static SDK::AHWCharacter_PlayableCharacter* SafePlayable(SDK::APawn* pawn,bool requireAlive,bool requireSpectatable=true)
    {
        __try {
            if(!Live(pawn)||!pawn->IsA(SDK::AHWCharacter_PlayableCharacter::StaticClass()))return nullptr;
            auto* p=static_cast<SDK::AHWCharacter_PlayableCharacter*>(pawn);
            if(!Live(p->CameraSpringArm)||!Live(p->Camera)||!Live(p->RootComponent))return nullptr;
            if(requireAlive){
                if((requireSpectatable&&!p->bCanSpectate)||p->IsDead())return nullptr;
                const float health=static_cast<float>(p->GetHealth());if(!std::isfinite(health)||health<=0.f)return nullptr;
            }
            return p;
        }__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    static int Roster(const Context& c)
    {
        __try {
            if(!Live(c.world->GameState))return 0;
            auto& a=c.world->GameState->PlayerArray;const int n=a.Num();
            if(n<0||n>64||a.Max()<n)return 0;
            for(int i=0;i<n;i++){
                if(!a.IsValidIndex(i))break;auto* ps=a[i];
                if(!Live(ps)||ps==c.ps||ps->bIsInactive||ps->bFromPreviousLevel)continue;
                auto* pawn=SafePlayable(ps->PawnPrivate,true);if(!pawn)continue;
                bool duplicate=false;for(int j=0;j<s_count;j++)if(s_entries[j].pawn==pawn)duplicate=true;
                if(!duplicate&&s_count<64)s_entries[s_count++]={pawn,ps};
            }
            return s_count;
        }__except(EXCEPTION_EXECUTE_HANDLER){return s_count;}
    }
    static SDK::AActor* View(const Context& c)
    {
        __try {auto* manager=c.pc->PlayerCameraManager;if(!Live(manager))return nullptr;return manager->ViewTarget.Target;}
        __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    static bool Switch(const Context& c,SDK::AHWCharacter_PlayableCharacter* target)
    {
        __try {
            // NO server spectator functions, possession, team edits or direct ViewTarget writes.
            Context now;if(!ReadContext(&now)||now.world!=c.world||now.pc!=c.pc||now.ps!=c.ps)return false;
            if(!SafePlayable(target,true,target!=c.pc->Pawn))return false;
            c.pc->SetViewTargetWithBlend(target,0.f,SDK::EViewTargetBlendFunction::VTBlend_Linear,1.f,false);
            return View(c)==target;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static SDK::AHWCharacter_PlayableCharacter* Resolve(const Identity& id)
    {
        for(int i=0;i<s_count;i++)if(Matches(id,s_entries[i].pawn))return s_entries[i].pawn;
        return nullptr;
    }
    static SDK::AHWCharacter_PlayableCharacter* NormalTarget(const Context& c)
    {
        __try {
            if(!c.dead)return SafePlayable(c.pc->Pawn,true,false);
            if(c.module){auto* ps=c.module->SpectatedPlayerState.Get();if(Live(ps))return SafePlayable(ps->PawnPrivate,true);}
        }__except(EXCEPTION_EXECUTE_HANDLER){}
        return nullptr;
    }
    static bool ReleaseLookLease()
    {
        __try {
            if(!s_lookPending.load())return true;
            auto* world=Helpers::GetWorld();
            // Never dereference identities from another World, or root a World through this module.
            if(Live(world)&&!Matches(s_look.world,world)){s_look={};s_lookPending=false;return true;}
            if(!Live(world))return false;
            auto* obj=SDK::UObject::GObjects->GetByIndex(s_look.arm.index);
            if(!Live(obj)||!Matches(s_look.arm,obj)){s_look={};s_lookPending=false;return true;}
            Context c;if(!ReadContext(&c))return false;
            auto* normal=NormalTarget(c);
            // The game has made this exact pawn the native spectator target: hand off its flags.
            if(c.dead&&normal&&Matches(s_look.pawn,normal)){
                s_look={};s_lookPending=false;return true;
            }
            auto* arm=static_cast<SDK::UHWSpringArmComponent*>(obj);
            // All owned bits must still match our applied values. Any external change wins.
            if((s_look.localWritten&&!arm->bUseLocalControlRotation)||
               (s_look.pawnWritten&&arm->bUsePawnControlRotation)){
                s_look={};s_lookPending=false;return true;
            }
            if(s_look.localWritten)arm->bUseLocalControlRotation=s_look.oldLocal;
            if(s_look.pawnWritten)arm->bUsePawnControlRotation=s_look.oldPawn;
            s_look={};s_lookPending=false;return true;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static void ObserveNativeLookProfile(const Context& c)
    {
        __try {
            if(s_nativeLookReady.load()&&s_profileMode!=static_cast<int>(c.pc->CurrentCameraMode))s_nativeLookReady=false;
            if(s_nativeLookReady.load()||s_lookPending.load()||!c.dead||!c.module)return;
            auto* normal=NormalTarget(c);
            if(!normal||View(c)!=normal)return;
            auto* arm=normal->CameraSpringArm;
            // This profile was observed in the user's working native ally-camera logs.
            if(Live(arm)&&arm->bIsCurrentLocalViewTarget&&arm->bUseLocalControlRotation&&!arm->bUsePawnControlRotation)
                {s_profileMode=static_cast<int>(c.pc->CurrentCameraMode);s_nativeLookReady=true;}
        }__except(EXCEPTION_EXECUTE_HANDLER){}
    }
    static bool ApplyNativeLook(const Context& c,SDK::AHWCharacter_PlayableCharacter* target)
    {
        __try {
            if(!s_nativeLookReady.load()||s_lookPending.load())return false;
            Context now;if(!ReadContext(&now)||now.world!=c.world||now.pc!=c.pc||!now.dead||!now.module)return false;
            if(View(now)!=target||!SafePlayable(target,true))return false;
            if(NormalTarget(now)==target)return true; // Native target stays entirely under game control.
            auto* arm=target->CameraSpringArm;
            if(!Live(arm)||!arm->bIsCurrentLocalViewTarget)return false;
            const bool oldLocal=arm->bUseLocalControlRotation,oldPawn=arm->bUsePawnControlRotation;
            if(oldLocal&&!oldPawn)return true; // Already uses the working native profile: no ownership.
            s_look={};s_look.world=Id(now.world);s_look.pawn=Id(target);s_look.arm=Id(arm);
            s_look.oldLocal=oldLocal;s_look.oldPawn=oldPawn;s_lookPending=true;
            // Mark ownership before each write, so an interrupted update remains tracked.
            s_look.localWritten=true;arm->bUseLocalControlRotation=true;
            s_look.pawnWritten=true;arm->bUsePawnControlRotation=false;
            return arm->bUseLocalControlRotation&&!arm->bUsePawnControlRotation;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static bool NativeLookStillOwned()
    {
        __try {
            if(!s_lookPending.load())return true;
            auto* obj=SDK::UObject::GObjects->GetByIndex(s_look.arm.index);
            if(!Live(obj)||!Matches(s_look.arm,obj))return false;
            auto* arm=static_cast<SDK::UHWSpringArmComponent*>(obj);
            return arm->bUseLocalControlRotation&&!arm->bUsePawnControlRotation;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static bool Restore(const Context& c)
    {
        if(!ReleaseLookLease()){Publish("Camera flag restore pending; unload blocked");return false;}
        if(!s_active.load())return true;
        auto* current=View(c);
        // If the game has already reclaimed the camera, do not overwrite its new decision.
        if(!Live(current)){Publish("Restore pending: camera target unavailable");return false;}
        if(!Matches(s_chosen,current)){Forget("Camera returned to the game");return true;}
        auto* normal=NormalTarget(c);if(!normal)normal=Resolve(s_saved);
        if(normal&&Switch(c,normal)){Forget("Normal camera restored");return true;}
        Publish("Restore pending: use normal game spectate controls or wait for respawn");return false;
    }
    static void PrintNativeStateSafe()
    {
        // Read-only diagnostics, on the previously verified engine/window execution thread.
        __try {
            Context c;if(!ReadContext(&c)){printf("[DeathNative] No valid camera context.\n");return;}
            auto* view=View(c);auto* normal=NormalTarget(c);
            printf("\n[DeathNative] SAMPLE %u | dead=%d customTarget=%d nativeModule=%p view=%p normal=%p\n",
                ++s_samples,c.dead?1:0,s_active.load()?1:0,static_cast<void*>(c.module),static_cast<void*>(view),static_cast<void*>(normal));
            printf("[DeathNative] pc=%p cursor=%d mode=%d aimPitch=%.3f control=(%.3f,%.3f,%.3f) deathModifier=%p\n",
                static_cast<void*>(c.pc),c.pc->bShowMouseCursor?1:0,static_cast<int>(c.pc->CurrentCameraMode),
                c.pc->CameraPitchAim,c.pc->ControlRotation.Pitch,c.pc->ControlRotation.Yaw,c.pc->ControlRotation.Roll,
                static_cast<void*>(c.pc->DeathSpectatorCameraMod));
            if(!Live(view)||!view->IsA(SDK::AHWCharacter_PlayableCharacter::StaticClass())){
                printf("[DeathNative] View is not a valid playable character; no arm snapshot.\n");return;
            }
            auto* pawn=static_cast<SDK::AHWCharacter_PlayableCharacter*>(view);auto* arm=pawn->CameraSpringArm;
            if(!Live(arm)){printf("[DeathNative] View has no valid spring arm.\n");return;}
            const auto rotation=arm->GetTargetRotation();
            printf("[DeathNative] pawnIndex=%d armIndex=%d localRotation=%d localView=%d pawnControl=%d inherit=(%d,%d,%d)\n",
                pawn->Index,arm->Index,arm->bUseLocalControlRotation?1:0,arm->bIsCurrentLocalViewTarget?1:0,
                arm->bUsePawnControlRotation?1:0,arm->bInheritPitch?1:0,arm->bInheritYaw?1:0,arm->bInheritRoll?1:0);
            printf("[DeathNative] armLength=%.3f armRelative=(%.3f,%.3f,%.3f) armTarget=(%.3f,%.3f,%.3f) collision=%d lag=%d rotLag=%d\n",
                static_cast<double>(arm->TargetArmLength),arm->RelativeRotation.Pitch,arm->RelativeRotation.Yaw,arm->RelativeRotation.Roll,
                rotation.Pitch,rotation.Yaw,rotation.Roll,arm->bDoCollisionTest?1:0,arm->bEnableCameraLag?1:0,arm->bEnableCameraRotationLag?1:0);
            auto* settings=pawn->CurrentCameraSettings;
            if(Live(settings))printf("[DeathNative] settingsIndex=%d cameraType=%d fov=%.3f zoomArms=(%.3f,%.3f)\n",
                settings->Index,static_cast<int>(settings->CameraType),static_cast<double>(settings->FOV),
                static_cast<double>(settings->SpringArmLength_MaxZoom),static_cast<double>(settings->SpringArmLength_MinZoom));
            printf("[DeathNative] profileReady=%d flagLease=%d | native rotation input passed through, not generated or scaled\n",s_nativeLookReady.load()?1:0,s_lookPending.load()?1:0);
            printf("[DeathNative] END SAMPLE\n");
        }__except(EXCEPTION_EXECUTE_HANDLER){printf("[DeathNative] Snapshot unavailable: guarded read failed.\n");}
    }
    static void TargetName(SDK::APlayerState* ps,char* out,int size)
    {
        wchar_t wide[64]{};
        __try {
            const auto& text=ps->PlayerNamePrivate;int n=text.Num();
            if(n<1||n>1024||text.Max()<n||!Helpers::IsValidPtr(text.GetDataPtr())){snprintf(out,size,"Player");return;}
            int j=0;for(;j<n&&j<63;j++){wchar_t c=text.GetDataPtr()[j];if(!c)break;wide[j]=c<32?L' ':c;}wide[j]=0;
        }__except(EXCEPTION_EXECUTE_HANDLER){snprintf(out,size,"Player");return;}
        if(!WideCharToMultiByte(CP_UTF8,0,wide,-1,out,size,nullptr,nullptr))snprintf(out,size,"Player");
    }
    static void Tick()
    {
        // Fail closed until a real local OnRep_IsDeathSpectating callback establishes
        // the execution thread. A mere HWND-owner check is NOT sufficient for mutation.
        if(!s_gameThread.load()||s_gameThread.load()!=GetCurrentThreadId()){
            s_deadReady=false;Publish("Waiting for engine death callback / verified game-window thread");s_command=0;s_pendingClicks=0;return;
        }
        Context c;if(!ReadContext(&c)){s_deadReady=false;Publish("No valid player camera (lobby / transition)");s_command=0;s_pendingClicks=0;return;}
        if(s_observedPC.load()!=reinterpret_cast<uintptr_t>(c.pc)){
            Forget("Controller changed: waiting for its death callback");s_command=0;s_pendingClicks=0;return;
        }
        if(!Same(s_world,Id(c.world))||!Same(s_pc,Id(c.pc))){
            Forget("Camera context changed");s_world=Id(c.world);s_pc=Id(c.pc);s_nativeLookReady=false;s_nextRoster=0;
        }
        if(!c.dead)s_nativeLookReady=false;
        ObserveNativeLookProfile(c);
        if(s_capture.exchange(false))PrintNativeStateSafe();
        ClearScratch();
        if(!s_enabled.load()||s_suspend.load()||!c.dead||!c.module){
            s_deadReady=false;if(s_active.load())Roster(c);Restore(c);if(!s_active.load())Publish(!s_enabled.load()?"Disabled":"Waiting for death-spectator mode");
            s_command=0;s_pendingClicks=0;ClearScratch();return;
        }
        s_deadReady=true;
        if(s_active.load()&&Live(View(c))&&!Matches(s_chosen,View(c))){
            Forget("Game overrode the camera; no automatic retry");s_command=0;s_pendingClicks=0;ClearScratch();return;
        }
        if(s_active.load()&&!s_nativeLookReady.load()){
            Roster(c);Restore(c);Publish("Native camera mode changed; waiting for a new ally profile");ClearScratch();return;
        }
        if(s_active.load()&&!NativeLookStillOwned()){
            Restore(c);Publish("Game changed camera look flags; native camera requested (no reapply)");ClearScratch();return;
        }
        int command=s_command.exchange(0);int advance=1;
        const int queuedClicks=s_pendingClicks.exchange(0);
        const int clicks=(Menu::IsOpen||GameUiOwnsMouse(c))?0:queuedClicks;
        if(command==0&&clicks>0){command=1;advance=clicks;}
        if(command==2){if(s_active.load())Roster(c);Restore(c);ClearScratch();return;}
        if(command!=1&&command!=-1){
            if(GetTickCount64()>=s_nextRoster){
                Roster(c);s_nextRoster=GetTickCount64()+1000;
                if(!s_active.load())Publish(s_nativeLookReady.load()?"Ready: native ally-look profile captured / LMB next":"Waiting for native ally camera (localRotation=1, pawnControl=0)",nullptr,s_count);
                else {std::lock_guard<std::mutex> lock(s_mutex);s_candidates=s_count;}
            }
            ClearScratch();return;
        }
        if(!s_nativeLookReady.load()){Publish("First observe a native ally camera; no camera flags changed");ClearScratch();return;}
        Roster(c); // Revalidate live targets immediately before a requested switch.
        if(!s_count){Publish("No available living player cameras",nullptr,0);ClearScratch();return;}
        int at=command>0?-1:0;auto* current=View(c);
        for(int i=0;i<s_count;i++)if(s_entries[i].pawn==current){at=i;break;}
        const int next=(at+command*(advance%s_count)+s_count)%s_count;
        auto* target=s_entries[next].pawn;
        if(!Live(current)){Publish("Current camera unavailable; switch skipped");ClearScratch();return;}
        if(target==current){Publish("Only the current player camera is available");ClearScratch();return;}
        Identity before{};
        for(int i=0;i<s_count;i++)if(s_entries[i].pawn==current)before=Id(current);
        // Require a restorable original player camera; never capture cinematics/structures.
        if(!s_active.load()&&!before.address){Publish("Normal player camera unavailable; switch skipped");ClearScratch();return;}
        if(!ReleaseLookLease()){Publish("Previous camera flags need restoring; switch skipped");ClearScratch();return;}
        const bool wasActive=s_active.load();const Identity previous=s_chosen;
        // Track ownership before calling engine code, including a partially completed call.
        if(!wasActive)s_saved=before;s_chosen=Id(target);s_active=true;
        if(Switch(c,target)){
            if(ApplyNativeLook(c,target)){
                char name[192]{};TargetName(s_entries[next].ps,name,sizeof(name));Publish("Native look flags active: LMB next / original game mouse input / F8 restore",name,s_count);
            }else {
                Restore(c);Publish("Native look setup unavailable; normal camera requested");
            }
        }else {
            auto* after=View(c);
            if(after==current){
                if(wasActive){s_chosen=previous;auto* old=Resolve(previous);if(old&&!ApplyNativeLook(c,old))Restore(c);}
                else Forget("Camera change rejected; no automatic retry");
            }
            Publish("Camera change not confirmed; restore if needed / no automatic retry");
        }
        ClearScratch();
    }
    static void TickSafe()
    {
        __try {Tick();}
        __except(EXCEPTION_EXECUTE_HANDLER){ClearScratch();Publish("Camera access failed; mutation skipped");}
    }
    struct Call {Call(){++s_calls;}~Call(){--s_calls;}};
    // Keep SEH in a POD-only function (MSVC C2712: no RAII in __try functions).
    static void ObserveDeathSafe(void* object)
    {
        __try {
            auto* pc=Helpers::GetLocalPC();
            if(pc&&pc==object&&Live(pc)&&pc->IsA(SDK::AHWPlayerController_Smite::StaticClass())){
                s_observedPC=reinterpret_cast<uintptr_t>(pc);s_gameThread=GetCurrentThreadId();
            }
        }__except(EXCEPTION_EXECUTE_HANDLER){}
    }
    static void DeathNotify(void* object,void* stack,void* result)
    {
        Call call;if(s_original)s_original(object,stack,result);
        if(!s_stopping.load())ObserveDeathSafe(object);
    }
    static bool Install()
    {
        if(s_hookReady.load())return true;
        void* address=nullptr;
        __try {
            auto* cls=SDK::AHWPlayerController_Smite::StaticClass();if(!Live(cls))return false;
            auto* fn=cls->GetFunction("HWPlayerController_Smite","OnRep_IsDeathSpectating");
            if(!Live(fn)||!fn->ExecFunction)return false;address=reinterpret_cast<void*>(fn->ExecFunction);
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
        if(MH_CreateHook(address,reinterpret_cast<void*>(&DeathNotify),reinterpret_cast<void**>(&s_original))!=MH_OK)return false;
        if(MH_EnableHook(address)!=MH_OK){MH_RemoveHook(address);s_original=nullptr;return false;}
        s_hookTarget=address;s_hookReady=true;return true;
    }
    static bool MouseClick(HWND hwnd,UINT msg)
    {
        // Deliberately no WM_INPUT / WM_MOUSEMOVE handling: native look is untouched.
        if(msg!=WM_LBUTTONDOWN&&msg!=WM_LBUTTONDBLCLK&&msg!=WM_LBUTTONUP)return false;
        // Check UI ownership BEFORE consuming even a captured release. A shop opened
        // during a press must get its mouse events back, not a stale camera latch.
        if(Menu::IsOpen||GetForegroundWindow()!=hwnd||s_gameThread.load()!=GetCurrentThreadId()){
            s_leftCaptured=false;s_pendingClicks=0;return false;
        }
        Context c;
        if(!ReadContext(&c)||!Matches(s_world,c.world)||!Matches(s_pc,c.pc)||GameUiOwnsMouse(c)){
            s_leftCaptured=false;s_pendingClicks=0;return false;
        }
        if(msg==WM_LBUTTONUP&&s_leftCaptured.exchange(false))return true;
        if(!s_enabled.load()||!s_deadReady.load()||s_suspend.load()||!c.dead||!c.module)return false;
        if(msg==WM_LBUTTONDOWN||msg==WM_LBUTTONDBLCLK){
            if(!s_leftCaptured.exchange(true)){
                int old=s_pendingClicks.load();while(!s_pendingClicks.compare_exchange_weak(old,std::min(old+1,16))){}
            }
            return true;
        }
        return false;
    }
    bool HandleWindowMessage(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
    {
        if(s_stopping.load())return false;
        if(MouseClick(hwnd,msg))return true;
        const UINT setup=s_setup.load();const bool isSetup=setup&&msg==setup;
        const bool timer=msg==WM_TIMER&&wp==TimerId;
        const bool key=(msg==WM_KEYDOWN||msg==WM_KEYUP)&&!Menu::IsOpen&&GetForegroundWindow()==hwnd&&s_enabled.load()&&s_deadReady.load()&&!s_suspend.load()&&s_hookReady.load()&&s_gameThread.load()==GetCurrentThreadId();
        if(key&&(wp==VK_OEM_4||wp==VK_OEM_6||wp==VK_F8)){
            if(msg==WM_KEYDOWN&&!(lp&(1LL<<30)))s_command=wp==VK_F8?2:wp==VK_OEM_4?-1:1;
            return true;
        }
        if(!isSetup&&!timer)return false;
        Call call;std::lock_guard<std::mutex> dispatch(s_dispatch);
        if(s_stopping.load())return true;
        if(GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId())return true;
        if(!s_enabled.load()&&!s_active.load()&&!s_lookPending.load()){KillTimer(hwnd,TimerId);s_window=nullptr;s_command=0;s_pendingClicks=0;Publish("Disabled","",0);return true;}
        if(isSetup){
            HWND old=s_window.load();if(old&&old!=hwnd)KillTimer(old,TimerId);
            if(!Install()){Publish("Death callback hook unavailable; camera disabled");return true;}
            if(SetTimer(hwnd,TimerId,100,nullptr))s_window=hwnd;return true;
        }
        static thread_local bool polling=false;if(polling)return true;
        polling=true;TickSafe();s_lastTick=GetTickCount64();polling=false;return true;
    }
    void Service()
    {
        if(s_stopping.load()||(!s_enabled.load()&&!s_active.load()&&!s_lookPending.load()))return;
        HWND hwnd=Hooks::g_hWnd;if(!hwnd||!IsWindow(hwnd))return;
        const uint64_t now=GetTickCount64();if(s_window.load()==hwnd&&now-s_lastTick.load()<2000)return;
        if(now-s_lastPost<1000)return;s_lastPost=now;
        if(!s_setup.load())s_setup=RegisterWindowMessageW(L"ALWAYSLOSE.DeathCamera.Experimental.v1");
        const UINT setup=s_setup.load();if(setup)PostMessageW(hwnd,setup,0,0);
    }
    void DrawSettings()
    {
        UITheme::BeginCard("death_camera",UITheme::Icon::Visuals,"Native death camera - local look flags");
        bool enabled=s_enabled.load();if(UITheme::CheckboxRow("Enable local death target cycling",&enabled))SetConfig({enabled});
        UITheme::InfoText("Uses the original game camera and mouse input. Applies the observed ally look profile to the selected local spring arm: localRotation=1, pawnControl=0. No separate orbit camera.");
        ImGui::BeginDisabled(!enabled||!s_deadReady.load()||s_suspend.load());
        if(ImGui::Button("Previous##death",{UITheme::Px(120.f),UITheme::Px(36.f)}))s_command=-1;UITheme::HelpLastItem("Previous","Previous available living player camera. Shortcut: [ with the menu closed.");UITheme::SameLineIfRoom(120.f);
        if(ImGui::Button("Next##death",{UITheme::Px(120.f),UITheme::Px(36.f)}))s_command=1;UITheme::HelpLastItem("Next","Next available living player camera. Shortcut: LMB or ] with the menu closed.");UITheme::SameLineIfRoom(120.f);
        ImGui::EndDisabled();ImGui::BeginDisabled(!s_active.load());
        if(ImGui::Button("Restore##death",{UITheme::Px(120.f),UITheme::Px(36.f)}))s_command=2;
        UITheme::HelpLastItem("Restore","Return camera control using the existing restore path. Shortcut: F8. Restore or respawn before unloading.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!enabled||!s_gameThread.load());
        if(ImGui::Button("Print native camera state",{fminf(ImGui::GetContentRegionAvail().x,UITheme::Px(260.f)),UITheme::Px(36.f)}))s_capture=true;
        UITheme::HelpLastItem("Print native camera state","Print a read-only camera snapshot to the console for troubleshooting.");
        ImGui::EndDisabled();
        UITheme::InfoText("Enable before dying and wait on the game's native ally camera until Ready. If rotation still differs on an enemy, print before/after moving the mouse and send both [DeathNative] blocks.");
        std::string status,target;int count;
        {std::lock_guard<std::mutex> lock(s_mutex);status=s_status;target=s_target;count=s_candidates;}
        ImGui::TextWrapped("Status: %s",status.c_str());if(!target.empty())ImGui::TextWrapped("Target: %s",target.c_str());
        ImGui::Text("Available player cameras: %d",count);
        UITheme::InfoText("LMB next, [ previous, ] next, F8 restore. Only two local spring-arm flags are scoped and restored. Native spectator lists/indices, teams, possession, sensitivity, zoom and FOV are not edited.");
        UITheme::EndCard();
    }
    bool PrepareUnload()
    {
        {std::lock_guard<std::mutex> dispatch(s_dispatch);s_suspend=true;s_deadReady=false;s_pendingClicks=0;s_capture=false;s_command=2;}
        // The lock waits for an in-flight switch; suspended ticks can only restore.
        if(!s_active.load()&&!s_lookPending.load())return true;
        s_lastTick=0; // request dispatcher progress; Service() itself is Present-thread-only
        const UINT setup=s_setup.load();HWND hwnd=s_window.load();if(hwnd&&setup)PostMessageW(hwnd,setup,0,0);
        for(int i=0;i<100&&(s_active.load()||s_lookPending.load());i++)Sleep(10);
        if(s_active.load()||s_lookPending.load()){Publish("Unload refused: restore camera first / use normal spectate / respawn");s_suspend=false;return false;}
        return true;
    }
    void Shutdown()
    {
        {std::lock_guard<std::mutex> dispatch(s_dispatch);
            s_stopping=true;HWND hwnd=s_window.exchange(nullptr);if(hwnd)KillTimer(hwnd,TimerId);
            if(s_hookTarget)MH_DisableHook(s_hookTarget);
        }
        while(s_calls.load())Sleep(1);
        if(s_hookTarget)MH_RemoveHook(s_hookTarget);
        s_hookTarget=nullptr;s_hookReady=false;s_original=nullptr;
    }
}
