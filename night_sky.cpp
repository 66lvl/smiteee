#include "night_sky.h"
#include "../core/sdk_helpers.h"
// SDK.hpp excludes parameter headers in normal builds; these layouts are used below.
#include "../CppSDK/SDK/Engine_parameters.hpp"
#include "../ui/theme.h"
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <string>

namespace NightSky
{
    namespace
    {
        // Deliberately NOT saved in profiles: explicit opt-in for every session.
        std::atomic_bool s_enabled{false},s_owned{false},s_retry{false},s_requested{false},s_applied{false};
        std::mutex s_statusMutex;
        std::string s_status="Disabled - original sky/lighting",s_map="Not checked yet";
        constexpr int LightCount=5;
        struct Ref
        {
            SDK::UObject* Pointer=nullptr;SDK::UClass* Class=nullptr;SDK::UObject* Outer=nullptr;
            SDK::FName Name{};int Index=-1;
        };
        struct Light
        {
            Ref Object,Owner,Level;bool Sky=false,Sun=false,IntensityOwned=false,RotationOwned=false;
            float Original=0.f,Written=0.f;
            SDK::FRotator OriginalRotation{},WrittenRotation{};
        };
        Light s_lights[LightCount]{};
        Ref s_world,s_level;
        bool s_faulted=false;
        void Status(const std::string& text){std::lock_guard<std::mutex> lock(s_statusMutex);s_status=text;}
        void MapName(const std::string& name){std::lock_guard<std::mutex> lock(s_statusMutex);s_map=name;}
        void Request(bool enabled)
        {
            // UI preference is distinct from runtime acceptance. A rejection must not
            // instantly undo the user's click or pretend the preset was applied.
            s_requested=enabled;s_enabled=enabled;s_retry=enabled;
            if(enabled)Status("Request queued - waiting for the window dispatcher");
        }
        bool Live(SDK::UObject* o)
        {
            if(!Helpers::IsValidPtr(o)||!Helpers::IsValidPtr(o->Class))return false;
            const auto bad=static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject)|static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
            return !(static_cast<uint32_t>(o->Flags)&bad);
        }
        Ref Identity(SDK::UObject* object)
        {return {object,object->Class,object->Outer,object->Name,object->Index};}
        SDK::UObject* Resolve(const Ref& ref)
        {
            if(ref.Index<0)return nullptr;
            auto* object=SDK::UObject::GObjects->GetByIndex(ref.Index);
            if(object!=ref.Pointer||!Live(object)||object->Class!=ref.Class||object->Outer!=ref.Outer||object->Name!=ref.Name)return nullptr;
            return object;
        }
        bool SameFloat(float a,float b)
        {return std::isfinite(a)&&std::isfinite(b)&&std::fabs(a-b)<0.0001f;}
        bool Angle(double a,double b)
        {return std::isfinite(a)&&std::isfinite(b)&&std::fabs(std::remainder(a-b,360.0))<0.01;}
        bool SameRotation(const SDK::FRotator& a, const SDK::FRotator& b)
        {
            auto finite = [](const SDK::FRotator& r) {
                return std::isfinite(r.Pitch) &&
                    std::isfinite(r.Yaw) &&
                    std::isfinite(r.Roll);
                };
            if (!finite(a) || !finite(b))
                return false;

            struct Q { double X, Y, Z, W; };

            auto quaternion = [](const SDK::FRotator& r) -> Q {
                constexpr double k = 3.14159265358979323846 / 360.0;
                const double p = std::remainder(r.Pitch, 360.0) * k;
                const double y = std::remainder(r.Yaw, 360.0) * k;
                const double z = std::remainder(r.Roll, 360.0) * k;

                const double sp = std::sin(p), cp = std::cos(p);
                const double sy = std::sin(y), cy = std::cos(y);
                const double sr = std::sin(z), cr = std::cos(z);

                return {
                    cr * sp * sy - sr * cp * cy,
                    -cr * sp * cy - sr * cp * sy,
                    cr * cp * sy - sr * sp * cy,
                    cr * cp * cy + sr * sp * sy
                };
                };

            const auto q = quaternion(a), r = quaternion(b);
            const double norm = std::sqrt(
                (q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W) *
                (r.X * r.X + r.Y * r.Y + r.Z * r.Z + r.W * r.W)
            );
            if (!std::isfinite(norm) || norm <= 0.0)
                return false;

            double dot = std::fabs(
                (q.X * r.X + q.Y * r.Y + q.Z * r.Z + q.W * r.W) / norm
            );
            if (dot > 1.0)
                dot = 1.0;

            return 2.0 * std::acos(dot) *
                (180.0 / 3.14159265358979323846) <= 0.01;
        }
        bool FiniteRotation(const SDK::FRotator& r)
        {return std::isfinite(r.Pitch)&&std::isfinite(r.Yaw)&&std::isfinite(r.Roll);}
        SDK::UFunction* LocalNative(SDK::UObject* object,const char* owner,const char* name)
        {
            if(!Live(object))return nullptr;
            auto* function=object->Class->GetFunction(owner,name);
            if(!Live(function)||!function->IsA(SDK::UFunction::StaticClass()))return nullptr;
            const auto flags=static_cast<uint32_t>(function->FunctionFlags);
            const auto native=static_cast<uint32_t>(SDK::EFunctionFlags::Native);
            const auto forbidden=static_cast<uint32_t>(SDK::EFunctionFlags::Net)|static_cast<uint32_t>(SDK::EFunctionFlags::NetReliable)|static_cast<uint32_t>(SDK::EFunctionFlags::NetValidate)|static_cast<uint32_t>(SDK::EFunctionFlags::NetServer)|
                static_cast<uint32_t>(SDK::EFunctionFlags::NetClient)|static_cast<uint32_t>(SDK::EFunctionFlags::NetMulticast)|
                static_cast<uint32_t>(SDK::EFunctionFlags::NetRequest)|static_cast<uint32_t>(SDK::EFunctionFlags::NetResponse)|
                static_cast<uint32_t>(SDK::EFunctionFlags::BlueprintAuthorityOnly)|static_cast<uint32_t>(SDK::EFunctionFlags::Static);
            return (flags&native)&&!(flags&forbidden)?function:nullptr;
        }
        bool Intensity(SDK::ULightComponentBase* component,bool sky,float value)
        {
            auto* fn=LocalNative(component,sky?"SkyLightComponent":"LightComponent","SetIntensity");
            if(!fn)return false;
            // Use generated SDK parameter layouts, but do NOT alter UFunction flags.
            if(sky){SDK::Params::SkyLightComponent_SetIntensity params{};params.NewIntensity=value;component->ProcessEvent(fn,&params);}
            else {SDK::Params::LightComponent_SetIntensity params{};params.NewIntensity=value;component->ProcessEvent(fn,&params);}
            return SameFloat(component->Intensity,value);
        }
        bool Rotation(SDK::USceneComponent* component,const SDK::FRotator& value)
        {
            auto* fn=LocalNative(component,"SceneComponent","K2_SetRelativeRotation");if(!fn)return false;
            SDK::Params::SceneComponent_K2_SetRelativeRotation params{};params.NewRotation=value;params.bSweep=false;params.bTeleport=true;
            component->ProcessEvent(fn,&params);return SameRotation(component->RelativeRotation,value);
        }
        template<class Array> bool ArrayOK(const Array& array,int limit)
        {return array.Num()>=0&&array.Num()<=limit&&array.Max()>=array.Num()&&array.Max()<=262144&&(!array.Num()||Helpers::IsValidPtr(array.GetDataPtr()));}
        void RefreshOwned()
        {
            bool owned=false;for(const auto& light:s_lights)owned|=light.IntensityOwned||light.RotationOwned;s_owned=owned;if(!owned)s_applied=false;
        }
        bool RestoreOne(Light& light)
        {
            if(!light.IntensityOwned&&!light.RotationOwned)return true;
            auto* raw=Resolve(light.Object);
            if(!raw){light.IntensityOwned=false;light.RotationOwned=false;return true;} // Old object no longer exists with this identity.
            auto* component=static_cast<SDK::ULightComponentBase*>(raw);
            if(light.RotationOwned)
            {
                if(SameRotation(component->RelativeRotation,light.OriginalRotation))light.RotationOwned=false;
                else if(!SameRotation(component->RelativeRotation,light.WrittenRotation))light.RotationOwned=false; // Preserve external edits.
                else if(Rotation(component,light.OriginalRotation))light.RotationOwned=false;
            }
            if(light.IntensityOwned)
            {
                if(SameFloat(component->Intensity,light.Original))light.IntensityOwned=false;
                else if(!SameFloat(component->Intensity,light.Written))light.IntensityOwned=false; // Preserve external edits.
                else if(Intensity(component,light.Sky,light.Original))light.IntensityOwned=false;
            }
            return !light.IntensityOwned&&!light.RotationOwned;
        }
        bool RestoreSafe(Light* light)
        {
            __try{return RestoreOne(*light);}
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
        bool Restore()
        {
            // Each entry has its own fault boundary, so one stale entry cannot starve others.
            bool okay=true;for(auto& light:s_lights)if(!RestoreSafe(&light))okay=false;
            RefreshOwned();return okay&&!s_owned.load();
        }
        bool Supported(SDK::UWorld* world)
        {return Live(world)&&world->Name.ToString()=="L_CQ_S1_Tutorial_P";}
        bool LevelLoaded(SDK::UWorld* world,SDK::UObject* level)
        {
            if(!Live(world)||!Live(level))return false;
            if(world->PersistentLevel==level)return true;
            auto levels=world->Levels;if(!ArrayOK(levels,64))return false;
            for(int i=0;i<levels.Num();++i)if(levels[i]==level)return true;
            return false;
        }
        bool Find(SDK::UWorld* world,Light* found,std::string& reason)
        {
            // All loaded levels, as in Sky Inspector. "PersistentLevel" is only an
            // object name and is not a unique identifier across streamed levels.
            auto* persistent=world->PersistentLevel;
            if(!Live(persistent)){reason="PersistentLevel unavailable";return false;}
            auto worldLevels=world->Levels;
            if(!ArrayOK(worldLevels,64)){reason="Loaded-level array invalid / more than 64 entries";return false;}
            SDK::ULevel* levels[64]{};int levelCount=0;
            auto add=[&](SDK::ULevel* level)
            {
                if(!Live(level))return;
                for(int i=0;i<levelCount;++i)if(levels[i]==level)return;
                if(levelCount<64)levels[levelCount++]=level;
            };
            add(persistent);for(int i=0;i<worldLevels.Num();++i)add(worldLevels[i]);
            // If persistent is absent from a full 64-entry list, do not silently truncate.
            if(worldLevels.Num()==64)
            {
                bool contains=false;for(int i=0;i<64;++i)if(worldLevels[i]==persistent)contains=true;
                if(!contains){reason="More than 64 unique loaded levels; refused";return false;}
            }
            struct LevelSnapshot
            {
                Ref Object;decltype(((SDK::ULevel*)nullptr)->Actors) Actors;
            } snapshots[64]{};
            const char* names[LightCount]={"DirectionalLight_3","DirectionalLight_0","DirectionalLight_4","DirectionalLight_5","SkyLight_2"};
            bool seen[LightCount]{};int atmosphereCount=0,actorSlots=0;
            SDK::UObject* atmosphereActor=nullptr;
            for(int l=0;l<levelCount;++l)
            {
                auto* level=levels[l];auto actors=level->Actors;
                snapshots[l].Object=Identity(level);snapshots[l].Actors=actors;
                if(Resolve(snapshots[l].Object)!=level){reason="Loaded level registry identity failed";return false;}
                if(!ArrayOK(actors,16384)){reason="Loaded level actor array invalid / over 16384 slots at level "+std::to_string(l);return false;}
                actorSlots+=actors.Num();if(actorSlots>32768){reason="More than 32768 actor slots; refused";return false;}
                for(int i=0;i<actors.Num();++i)
                {
                    auto* actor=actors[i];if(!Live(actor)||actor->Outer!=level)continue;
                    // Atmosphere is a presence check, not a mutation target. Do not
                    // require a session-specific _0 suffix or require it to be root.
                    if(actor->IsA(SDK::ASkyAtmosphere::StaticClass()))
                    {
                        auto* atmosphere=static_cast<SDK::ASkyAtmosphere*>(actor)->SkyAtmosphereComponent;
                        if(Live(atmosphere)&&atmosphere->IsA(SDK::USkyAtmosphereComponent::StaticClass())&&
                            atmosphere->Outer==actor&&!atmosphere->bHiddenInGame)
                        {
                            if(atmosphereActor!=actor){++atmosphereCount;atmosphereActor=actor;}
                        }
                    }
                    const bool sky=actor->IsA(SDK::ASkyLight::StaticClass());
                    if(!sky&&!actor->IsA(SDK::ADirectionalLight::StaticClass()))continue;
                    const auto name=actor->Name.ToString();int slot=-1;
                    for(int j=0;j<LightCount;++j)if(name==names[j]){slot=j;break;}
                    if(slot<0)continue;
                    if(seen[slot]||sky!=(slot==4)){reason=name+": duplicate name / unexpected actor type";return false;}
                    SDK::ULightComponentBase* component=sky?static_cast<SDK::ULightComponentBase*>(static_cast<SDK::ASkyLight*>(actor)->LightComponent):
                        static_cast<SDK::ULightComponentBase*>(static_cast<SDK::ADirectionalLight*>(actor)->LightComponent);
                    if(!Live(component)){reason=name+": light component unavailable";return false;}
                    if(component!=actor->RootComponent){reason=name+": light component is not the actor root";return false;}
                    if(component->Outer!=actor){reason=name+": component Outer is not the expected actor";return false;}
                    if(component->AttachParent){reason=name+": component has an attach parent; rotation context unverified";return false;}
                    if(static_cast<int>(component->Mobility)==0){reason=name+": Static mobility; runtime mutation refused";return false;}
                    if(!FiniteRotation(component->RelativeRotation)||!std::isfinite(component->Intensity)||component->Intensity<0.f||component->Intensity>10000.f)
                    {reason=name+": invalid rotation/intensity values";return false;}
                    if(sky){if(!component->IsA(SDK::USkyLightComponent::StaticClass())){reason=name+": expected SkyLightComponent";return false;}}
                    else
                    {
                        if(!component->IsA(SDK::UDirectionalLightComponent::StaticClass())){reason=name+": expected DirectionalLightComponent";return false;}
                        auto* sun=static_cast<SDK::UDirectionalLightComponent*>(component);
                        if(static_cast<bool>(sun->bAtmosphereSunLight)!=(slot==0)){reason=name+": atmosphereSun role differs from the inspected map";return false;}
                        if(slot==0&&sun->AtmosphereSunLightIndex!=0){reason=name+": sun index is not 0";return false;}
                    }
                    if(!LocalNative(component,sky?"SkyLightComponent":"LightComponent","SetIntensity")){reason=name+": local native SetIntensity unavailable/rejected by flags";return false;}
                    if(slot==0&&!LocalNative(component,"SceneComponent","K2_SetRelativeRotation")){reason=name+": local native K2_SetRelativeRotation unavailable/rejected by flags";return false;}
                    Light entry{};entry.Object=Identity(component);entry.Owner=Identity(actor);entry.Level=Identity(level);if(Resolve(entry.Object)!=component||Resolve(entry.Owner)!=actor||Resolve(entry.Level)!=level){reason=name+": registry identity verification failed";return false;}
                    entry.Sky=sky;entry.Sun=(slot==0);entry.Original=component->Intensity;
                    // Attenuation only: never increase light intensity, including an already-dark map.
                    entry.Written=entry.Original*(sky?0.15f:(slot==0?0.025f:0.01f));
                    entry.OriginalRotation=component->RelativeRotation;entry.WrittenRotation=entry.OriginalRotation;
                    if(entry.Sun)entry.WrittenRotation.Pitch=12.0; // Root has no attach parent; move the atmospheric sun below the horizon.
                    found[slot]=entry;seen[slot]=true;
                }
            }
            if(Helpers::GetWorld()!=world||world->PersistentLevel!=persistent||
                worldLevels.GetDataPtr()!=world->Levels.GetDataPtr()||worldLevels.Num()!=world->Levels.Num()||worldLevels.Max()!=world->Levels.Max())
            {reason="World/loaded-level array changed during preparation";return false;}
            for(int l=0;l<levelCount;++l)
            {
                auto* level=static_cast<SDK::ULevel*>(Resolve(snapshots[l].Object));
                if(!level||!LevelLoaded(world,level)||snapshots[l].Actors.GetDataPtr()!=level->Actors.GetDataPtr()||
                    snapshots[l].Actors.Num()!=level->Actors.Num()||snapshots[l].Actors.Max()!=level->Actors.Max())
                {reason="A loaded level/actor array changed during preparation";return false;}
            }
            if(atmosphereCount!=1)
            {reason="Expected exactly one readable, non-hidden typed SkyAtmosphere across loaded levels; found "+std::to_string(atmosphereCount);return false;}
            for(int i=0;i<LightCount;++i)if(!seen[i]){reason=std::string("Missing expected loaded-level light: ")+names[i];return false;}
            return true;
        }
        bool StillOwned(SDK::UWorld* world)
        {
            for(const auto& light:s_lights)
            {
                auto* raw=Resolve(light.Object);if(!raw)return false;
                auto* actor=Resolve(light.Owner);auto* level=Resolve(light.Level);
                if(!actor||!level||actor->Outer!=level||raw->Outer!=actor||!LevelLoaded(world,level))return false;
                auto* component=static_cast<SDK::ULightComponentBase*>(raw);
                if(component->AttachParent||!SameFloat(component->Intensity,light.Written))return false;
                if(light.Sun&&!SameRotation(component->RelativeRotation,light.WrittenRotation))return false;
            }
            return true;
        }
        void Tick(HWND hwnd,bool suspended)
        {
            if(suspended||!s_enabled.load()||GetForegroundWindow()!=hwnd||s_faulted)
            {
                s_applied=false;const bool okay=Restore();
                if(!okay)Status("Restore pending - do not unload; keep the game responsive and retry Restore");
                else if(s_faulted)Status("Stopped after an access fault; original/matching owned fields restored. Toggle to retry.");
                else if(!s_requested.load())Status("Disabled - original or external lighting preserved");
                else if(s_enabled.load())Status("Paused - game unfocused / unloading; no active sky preset");
                // A rejected request keeps its original, specific reason while restores finish.
                return;
            }
            auto* world=Helpers::GetWorld();
            MapName(Live(world)?world->Name.ToString():"No live world");
            if(!Supported(world))
            {
                s_enabled=false;s_applied=false;Restore();Status("Not applied: only the inspected L_CQ_S1_Tutorial_P map is supported");return;
            }
            if(s_owned.load())
            {
                if(Resolve(s_world)!=world||Resolve(s_level)!=world->PersistentLevel||!StillOwned(world))
                {s_enabled=false;s_applied=false;Restore();Status("Stopped: scene or lighting changed; matching owned values restored");}
                return; // Never fight the game's lighting or reapply on every tick.
            }
            Light found[LightCount]{};std::string reason;
            if(!Find(world,found,reason)){s_enabled=false;Status("Not applied: "+reason);return;}
            s_world=Identity(world);s_level=Identity(world->PersistentLevel);if(Resolve(s_world)!=world||Resolve(s_level)!=world->PersistentLevel){s_enabled=false;Status("World identity unavailable; no changes made");return;}
            for(int i=0;i<LightCount;++i)s_lights[i]=found[i];
            for(auto& light:s_lights)
            {
                if(Helpers::GetWorld()!=world||Resolve(s_world)!=world||Resolve(s_level)!=world->PersistentLevel){s_enabled=false;s_applied=false;Restore();Status("Map changed; restore requested");return;}
                auto* raw=Resolve(light.Object);auto* owner=Resolve(light.Owner);auto* level=Resolve(light.Level);
                if(!raw||!owner||!level||owner->Outer!=level||raw->Outer!=owner||!LevelLoaded(world,level))
                {s_enabled=false;s_applied=false;Restore();Status("Light/owning loaded level changed; restore requested");return;}
                auto* component=static_cast<SDK::ULightComponentBase*>(raw);
                // Recheck originals before claiming ownership; no writes during discovery.
                if(!SameFloat(component->Intensity,light.Original)||(light.Sun&&!SameRotation(component->RelativeRotation,light.OriginalRotation)))
                {s_enabled=false;s_applied=false;Restore();Status("Light changed during preparation; restore requested");return;}
                light.IntensityOwned=true;s_owned=true;
                if(!Intensity(component,light.Sky,light.Written))
                {s_enabled=false;s_applied=false;Restore();Status("Intensity setter not confirmed; restore requested");return;}
                if (light.Sun)
                {
                    light.RotationOwned = true;

                    if (!Rotation(component, light.WrittenRotation))
                    {
                        const auto actual = component->RelativeRotation;

                        auto angles = [](const SDK::FRotator& r) {
                            return std::string("(") +
                                std::to_string(r.Pitch) + ", " +
                                std::to_string(r.Yaw) + ", " +
                                std::to_string(r.Roll) + ")";
                            };

                        // Сохраняем результат вызова ДО восстановления.
                        const std::string detail =
                            std::string("Sun rotation not confirmed; mobility=") +
                            std::to_string(static_cast<int>(component->Mobility)) +
                            "; original=" + angles(light.OriginalRotation) +
                            "; requested=" + angles(light.WrittenRotation) +
                            "; actualBeforeRestore=" + angles(actual);

                        s_enabled = false;
                        s_applied = false;
                        Restore();
                        Status(detail);
                        return;
                    }
                }
            }
            s_applied=true;Status("Experimental sky preset applied; property readback confirmed, rendered result NOT verified");
        }
        bool TickSafe(HWND hwnd,bool suspended)
        {
            __try{Tick(hwnd,suspended);return true;}
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
    }
    bool NeedsService(){return s_enabled.load()||s_owned.load();}
    bool HasPendingRestore(){return s_owned.load();}
    void Disable(){s_requested=false;s_enabled=false;s_retry=false;}
    void TickOnWindowThread(HWND hwnd,bool suspended)
    {
        if(s_retry.exchange(false))s_faulted=false;
        if(!NeedsService())return;
        try
        {
            if(!TickSafe(hwnd,suspended)){s_enabled=false;s_applied=false;s_faulted=true;Restore();Status("Sky access fault - disabled; owned lighting restoration requested");}
        }
        catch(...){s_enabled=false;s_applied=false;s_faulted=true;Status("Sky operation failed - disabled; restoration pending if owned");}
    }
    void DrawPanel()
    {
        UITheme::BeginCard("world_night_sky",UITheme::Icon::Visuals,"Experimental night sky - inspected tutorial map only");
        bool enabled=s_requested.load();
        if(UITheme::CheckboxRow("Request experimental night sky",&enabled))
        {
            Request(enabled);
        }
        UITheme::InfoText("Changes the atmospheric sun direction and attenuates five inspected lights. No stars/moon assets, no foliage/material/visibility changes. Not the Night look camera filter.");
        if(ImGui::Button("Restore original sky / lighting",{UITheme::Px(290.f),UITheme::Px(36.f)}))Disable();
        std::string status,map;{std::lock_guard<std::mutex> lock(s_statusMutex);status=s_status;map=s_map;}
        ImGui::TextWrapped("Request: %s | Preset: %s",s_requested.load()?"ON":"OFF",s_applied.load()?"APPLIED (property readback only)":"NOT APPLIED");
        ImGui::TextWrapped("Last checked world: %s",map.c_str());
        ImGui::TextWrapped("Sky status: %s",status.c_str());
        ImGui::BeginDisabled(s_owned.load()||s_enabled.load());
        if(ImGui::Button("Retry sky request",{UITheme::Px(180.f),UITheme::Px(36.f)}))Request(true);
        ImGui::EndDisabled();
        if(ImGui::Button("Copy sky status",{UITheme::Px(180.f),UITheme::Px(36.f)}))
        {
            const std::string report=std::string("Night Sky loaded-level binding v1.2\nRequest: ")+(s_requested.load()?"ON":"OFF")+
                "\nPreset: "+(s_applied.load()?"APPLIED":"NOT APPLIED")+"\nWorld (last checked): "+map+"\nStatus: "+status+
                "\nPending owned fields: "+(s_owned.load()?"YES":"NO");
            ImGui::SetClipboardText(report.c_str());
        }
        UITheme::InfoText("Request ON does not mean the preset was applied. If NOT APPLIED, read/copy the reason above; retry explicitly after loading. No automatic retry on rejection.");
        if(!s_enabled.load()&&s_owned.load())ImGui::TextWrapped("Restoration pending: do not unload yet. Press Restore again if needed.");
        UITheme::InfoText("Only L_CQ_S1_Tutorial_P. Does not auto-enable from a saved profile. Disable before END; unload is refused while owned lighting restoration remains pending.");
        UITheme::EndCard();
    }
}
