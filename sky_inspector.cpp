#include "sky_inspector.h"
// Material inspection is optional and is not a dependency of attack recording.
#if __has_include("material_inspector.h") && __has_include("material_inspector_logic.h")
#include "material_inspector.h"
#define ALWAYSLOSE_HAS_MATERIAL_INSPECTOR 1
#else
#define ALWAYSLOSE_HAS_MATERIAL_INSPECTOR 0
#endif
#include "attack_inspector.h"
#include "basic_aim_trace.h"
#include "silent_aim_experiment.h"
#include "../core/sdk_helpers.h"
#include "../ui/theme.h"
#include <atomic>
#include <mutex>
#include <string>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>

namespace SkyInspector
{
    namespace
    {
        constexpr int MaxLevels=64, MaxActors=32768, MaxCandidates=48;
        constexpr int MaxComponents=48, MaxMaterials=8, MaxParameters=12, MaxParents=4;
        std::atomic_bool s_pending{false},s_busy{false};
        std::mutex s_mutex;
        std::string s_report,s_status="Not inspected";
        struct Capture
        {
            std::ostringstream Text;
            int Levels=0,Actors=0,Candidates=0,Faults=0,Components=0,MaterialSlots=0;
            bool Limited=false,Changed=false;
        };
        bool Live(SDK::UObject* object)
        {
            if(!Helpers::IsValidPtr(object)||!Helpers::IsValidPtr(object->Class))return false;
            const auto bad=static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject)|static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed)|static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
            return !(static_cast<uint32_t>(object->Flags)&bad);
        }
        std::string Name(SDK::UObject* object)
        {
            if(!Live(object))return "None/unreadable";
            auto text=object->Name.ToString();
            if(text.size()>180)text.resize(180);
            for(auto& ch:text)if(static_cast<unsigned char>(ch)<32)ch=' ';
            return text;
        }
        std::string Label(SDK::UObject* object)
        {
            if(!Live(object))return "None/unreadable";
            return Name(object)+" ["+Name(object->Class)+"]";
        }
        std::string ParameterName(const SDK::FName& name)
        {
            auto text=name.ToString();if(text.size()>100)text.resize(100);
            for(auto& ch:text)if(static_cast<unsigned char>(ch)<32)ch=' ';
            return text;
        }
        template<class Array> bool ValidArray(const Array& array,int maxHeader=262144)
        {
            const int n=array.Num(),capacity=array.Max();
            return n>=0&&n<=maxHeader&&capacity>=n&&capacity<=1048576&&(!n||Helpers::IsValidPtr(array.GetDataPtr()));
        }
        template<class Array> bool SameArray(const Array& a,const Array& b)
        {return a.GetDataPtr()==b.GetDataPtr()&&a.Num()==b.Num()&&a.Max()==b.Max();}
        bool Hint(std::string text)
        {
            std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            // Heuristic only. Deliberately do not match bare "sun"/"star" in god names.
            const char* keys[]={"sky","atmosphere","cloud","celestial","daynight","timeofday","sunlight","moonlight","skydome","starsphere"};
            for(const auto* key:keys)if(text.find(key)!=std::string::npos)return true;
            return false;
        }
        bool SkyComponent(SDK::USceneComponent* component)
        {
            return Live(component)&&(component->IsA(SDK::USkyAtmosphereComponent::StaticClass())||
                component->IsA(SDK::USkyLightComponent::StaticClass())||component->IsA(SDK::UDirectionalLightComponent::StaticClass()));
        }
        void Material(Capture& out,SDK::UMaterialInterface* material,int slot,const char* source)
        {
            if(!Live(material))return;
            if(out.MaterialSlots>=64||out.Text.tellp()>160000){out.Limited=true;return;}
            ++out.MaterialSlots;
            out.Text<<"      slot "<<slot<<" ("<<source<<"): "<<Label(material)<<"\n";
            SDK::UMaterialInterface* visited[MaxParents]{};int depth=0;
            while(Live(material)&&material->IsA(SDK::UMaterialInstance::StaticClass())&&depth<MaxParents)
            {
                for(int i=0;i<depth;++i)if(visited[i]==material){out.Text<<"        parent cycle; stopped\n";return;}
                visited[depth++]=material;
                auto* instance=static_cast<SDK::UMaterialInstance*>(material);
                auto scalar=instance->ScalarParameterValues;auto vector=instance->VectorParameterValues;
                out.Text<<"        instance depth="<<(depth-1)<<"; local override values (NOT effective shader values):\n";
                if(ValidArray(scalar,4096))
                {
                    if(scalar.Num()>MaxParameters){out.Limited=true;out.Text<<"        scalar parameters truncated\n";}
                    for(int i=0;i<scalar.Num()&&i<MaxParameters;++i)
                    {
                        const auto p=scalar[i];out.Text<<"          scalar "<<ParameterName(p.ParameterInfo.Name)
                            <<" {assoc="<<static_cast<int>(p.ParameterInfo.Association)<<", index="<<p.ParameterInfo.Index<<"}="<<p.ParameterValue<<"\n";
                    }
                    if(!SameArray(scalar,instance->ScalarParameterValues))out.Changed=true;
                }else out.Text<<"        scalar array invalid; skipped\n";
                if(ValidArray(vector,4096))
                {
                    if(vector.Num()>MaxParameters){out.Limited=true;out.Text<<"        vector parameters truncated\n";}
                    for(int i=0;i<vector.Num()&&i<MaxParameters;++i)
                    {
                        const auto p=vector[i];const auto v=p.ParameterValue;
                        out.Text<<"          vector "<<ParameterName(p.ParameterInfo.Name)<<" {assoc="<<static_cast<int>(p.ParameterInfo.Association)
                            <<", index="<<p.ParameterInfo.Index<<"}=("<<v.R<<","<<v.G<<","<<v.B<<","<<v.A<<")\n";
                    }
                    if(!SameArray(vector,instance->VectorParameterValues))out.Changed=true;
                }else out.Text<<"        vector array invalid; skipped\n";
                material=instance->Parent;out.Text<<"        parent: "<<Label(material)<<"\n";
            }
            if(depth==MaxParents&&Live(material)&&material->IsA(SDK::UMaterialInstance::StaticClass()))
            {out.Limited=true;out.Text<<"        parent depth limit reached\n";}
        }
        void Component(Capture& out,SDK::USceneComponent* component)
        {
            if(out.Components>=192||out.Text.tellp()>160000){out.Limited=true;return;}
            ++out.Components;
            out.Text<<"    component: "<<Label(component)<<" hiddenInGame="<<static_cast<int>(component->bHiddenInGame)<<"\n";
            const auto rotation=component->RelativeRotation;
            out.Text<<"      relative rotation=("<<rotation.Pitch<<","<<rotation.Yaw<<","<<rotation.Roll<<") mobility="<<static_cast<int>(component->Mobility)<<"\n";
            if(component->IsA(SDK::ULightComponentBase::StaticClass()))
            {
                auto* light=static_cast<SDK::ULightComponentBase*>(component);const auto color=light->LightColor;
                out.Text<<"      intensity="<<light->Intensity<<" colorRGB=("<<static_cast<int>(color.R)<<","<<static_cast<int>(color.G)<<","<<static_cast<int>(color.B)<<")\n";
            }
            if(component->IsA(SDK::UDirectionalLightComponent::StaticClass()))
            {
                auto* sun=static_cast<SDK::UDirectionalLightComponent*>(component);
                out.Text<<"      atmosphereSun="<<static_cast<int>(sun->bAtmosphereSunLight)<<" sunIndex="<<sun->AtmosphereSunLightIndex<<"\n";
            }
            if(component->IsA(SDK::USkyLightComponent::StaticClass()))
            {
                auto* sky=static_cast<SDK::USkyLightComponent*>(component);
                out.Text<<"      sourceType="<<static_cast<int>(sky->SourceType)<<" realTimeCapture="<<sky->bRealTimeCapture<<" cubemap="<<Label(sky->Cubemap)<<"\n";
            }
            if(component->IsA(SDK::USkyAtmosphereComponent::StaticClass()))
            {
                auto* sky=static_cast<SDK::USkyAtmosphereComponent*>(component);
                out.Text<<"      bottomRadius="<<sky->BottomRadius<<" atmosphereHeight="<<sky->AtmosphereHeight
                    <<" rayleighScale="<<sky->RayleighScatteringScale<<" mieScale="<<sky->MieScatteringScale<<"\n";
            }
            if(!component->IsA(SDK::UMeshComponent::StaticClass()))return;
            auto* mesh=static_cast<SDK::UMeshComponent*>(component);auto overrides=mesh->OverrideMaterials;
            const bool valid=ValidArray(overrides,4096);
            if(!valid)out.Text<<"      override materials array invalid; skipped\n";
            int slots=valid?overrides.Num():0;
            SDK::UStaticMesh* asset=nullptr;
            if(mesh->IsA(SDK::UStaticMeshComponent::StaticClass()))asset=static_cast<SDK::UStaticMeshComponent*>(mesh)->StaticMesh;
            if(Live(asset))
            {
                out.Text<<"      static mesh: "<<Label(asset)<<"\n";auto defaults=asset->StaticMaterials;
                if(ValidArray(defaults,4096))
                {
                    slots=(std::max)(slots,defaults.Num());
                    for(int i=0;i<slots&&i<MaxMaterials;++i)
                    {
                        auto* m=valid&&i<overrides.Num()?overrides[i]:nullptr;
                        if(m)Material(out,m,i,"override");else if(i<defaults.Num())Material(out,defaults[i].MaterialInterface,i,"static mesh default");
                    }
                    if(!SameArray(defaults,asset->StaticMaterials))out.Changed=true;
                }else out.Text<<"      static materials array invalid; skipped\n";
            }
            else if(valid)for(int i=0;i<slots&&i<MaxMaterials;++i)Material(out,overrides[i],i,"override");
            if(slots>MaxMaterials){out.Limited=true;out.Text<<"      material slot limit reached\n";}
            if(!SameArray(overrides,mesh->OverrideMaterials))out.Changed=true;
        }
        void Actor(Capture& out,SDK::AActor* actor)
        {
            if(out.Text.tellp()>160000){out.Limited=true;return;}
            if(!Live(actor)||actor->IsA(SDK::APawn::StaticClass())||actor->IsA(SDK::AController::StaticClass()))return;
            auto* root=actor->RootComponent;
            const bool typed=actor->IsA(SDK::ASkyAtmosphere::StaticClass())||actor->IsA(SDK::ASkyLight::StaticClass())||actor->IsA(SDK::ADirectionalLight::StaticClass())||SkyComponent(root);
            const bool hint=Hint(Name(actor)+" "+Name(actor->Class))||(Live(root)&&Hint(Name(root)+" "+Name(root->Class)));
            if(!typed&&!hint)return;
            ++out.Candidates;
            out.Text<<"\nCandidate "<<out.Candidates<<": "<<Label(actor)<<"\n  level: "<<Label(actor->Outer)
                <<"\n  match="<<(typed?"typed sky/light":"NAME HEURISTIC ONLY")<<"\n";
            SDK::USceneComponent* queue[MaxComponents]{};int count=0;
            auto add=[&](SDK::USceneComponent* value){if(!Live(value))return;for(int j=0;j<count;++j)if(queue[j]==value)return;
                if(count<MaxComponents)queue[count++]=value;else out.Limited=true;};
            add(root);
            if(actor->IsA(SDK::ASkyAtmosphere::StaticClass()))add(static_cast<SDK::ASkyAtmosphere*>(actor)->SkyAtmosphereComponent);
            if(actor->IsA(SDK::ASkyLight::StaticClass()))add(static_cast<SDK::ASkyLight*>(actor)->LightComponent);
            if(actor->IsA(SDK::ADirectionalLight::StaticClass()))add(static_cast<SDK::ADirectionalLight*>(actor)->LightComponent);
            for(int i=0;i<count;++i)
            {
                auto* component=queue[i];if(!Live(component))continue;
                Component(out,component);auto children=component->AttachChildren;
                if(!ValidArray(children,4096)){out.Text<<"      child array invalid; skipped\n";continue;}
                for(int j=0;j<children.Num()&&j<MaxComponents;++j)add(children[j]);
                if(children.Num()>MaxComponents)out.Limited=true;
                if(!SameArray(children,component->AttachChildren))out.Changed=true;
            }
            if(!count)out.Text<<"    no readable scene components\n";
        }
        bool ActorSafe(Capture* out,SDK::AActor* actor)
        {
            __try{Actor(*out,actor);return true;}
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
        void Collect(Capture& out)
        {
            auto* world=Helpers::GetWorld();
            out.Text<<"ALWAYSLOSE - Sky inspector v1 (READ ONLY)\nLocal client snapshot, not an atomic or server-authoritative snapshot.\n"
                <<"No sky/light/material changes, object spawning, ProcessEvent, RPC or asset loading.\n"
                <<"Scope: loaded levels; typed sky/light actors/root components and sky-like actor/root names.\n"
                <<"Generic actors with only non-root sky children may be missed. Name matches are NOT proof.\n"
                <<"Materials: up to 8 slots/component, 4 parent instances, 12 scalar + 12 vector overrides/instance.\n"
                <<"Global budgets: 64 levels, 32768 actor slots, 48 candidates, 192 components, 64 material slots, ~160 KB text.\n"
                <<"Values are local overrides, NOT resolved shader parameters; textures/functions are not inspected.\n"
                <<"Timestamp ms="<<GetTickCount64()<<"\n";
            if(!Live(world)||!Live(world->PersistentLevel)){out.Text<<"World unavailable; enter a match and inspect again.\n";return;}
            const auto worldName=world->Name;const int worldIndex=world->Index;
            auto* persistent=world->PersistentLevel;auto levels=world->Levels;
            out.Text<<"World: "<<Label(world)<<"\nPersistent level: "<<Label(persistent)<<"\n";
            if(!ValidArray(levels,4096)){out.Text<<"Level array invalid; no scan performed.\n";return;}
            SDK::ULevel* visited[MaxLevels]{};int count=0;
            auto add=[&](SDK::ULevel* level){if(!Live(level))return;for(int i=0;i<count;++i)if(visited[i]==level)return;
                if(count<MaxLevels)visited[count++]=level;else out.Limited=true;};
            add(persistent);for(int i=0;i<levels.Num()&&i<MaxLevels;++i)add(levels[i]);
            if(levels.Num()>MaxLevels)out.Limited=true;
            for(int l=0;l<count&&out.Actors<MaxActors&&out.Candidates<MaxCandidates;++l)
            {
                if(Helpers::GetWorld()!=world){out.Changed=true;break;}
                auto* level=visited[l];if(!Live(level))continue;auto actors=level->Actors;
                if(!ValidArray(actors)){++out.Faults;continue;}++out.Levels;
                for(int a=0;a<actors.Num();++a)
                {
                    if(out.Actors>=MaxActors||out.Candidates>=MaxCandidates||out.Text.tellp()>160000){out.Limited=true;break;}
                    ++out.Actors;if(!ActorSafe(&out,actors[a])){++out.Faults;out.Text<<"  [actor access fault; partial entry skipped]\n";}
                }
                if(!SameArray(actors,level->Actors))out.Changed=true;
            }
            if(out.Actors>=MaxActors||out.Candidates>=MaxCandidates)out.Limited=true;
            if(Helpers::GetWorld()!=world||!Live(world)||world->Name!=worldName||world->Index!=worldIndex||world->PersistentLevel!=persistent||!SameArray(levels,world->Levels))out.Changed=true;
            out.Text<<"\nSummary: levels="<<out.Levels<<" actor slots checked="<<out.Actors<<" candidates="<<out.Candidates
                <<" components="<<out.Components<<" materialSlots="<<out.MaterialSlots<<" access/array faults="<<out.Faults<<" limitsReached="<<out.Limited<<" changedDuringScan="<<out.Changed<<"\n"
                <<"Not a complete inventory. If changed/faulted/truncated, repeat while stationary after loading.\n";
        }
        bool CollectSafe(Capture* out)
        {
            __try{Collect(*out);return true;}
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
    }
    bool Pending()
    {
        bool materialPending=false;
#if ALWAYSLOSE_HAS_MATERIAL_INSPECTOR
        materialPending=MaterialInspector::Pending();
#endif
        return s_pending.load()||materialPending||AttackInspector::Pending()||SilentAimExperiment::NeedsService();
    }
    void CancelPending()
    {
        s_pending=false;
#if ALWAYSLOSE_HAS_MATERIAL_INSPECTOR
        MaterialInspector::CancelPending();
#endif
        AttackInspector::CancelPending();
        SilentAimExperiment::Disable();
    }
    void ProcessOnWindowThread()
    {
        AttackInspector::ProcessOnWindowThread();
        SilentAimExperiment::TickOnWindowThread();
#if ALWAYSLOSE_HAS_MATERIAL_INSPECTOR
        MaterialInspector::ProcessOnWindowThread();
#endif
        if(!s_pending.exchange(false))return;
        s_busy=true;
        try
        {
            Capture capture;capture.Text<<std::fixed<<std::setprecision(3);
            const bool complete=CollectSafe(&capture);
            if(!complete)capture.Text<<"\n[Access fault: PARTIAL report; repeat after map finishes loading.]\n";
            const auto report=capture.Text.str();
            std::lock_guard<std::mutex> lock(s_mutex);s_report=report;
            s_status=complete?"Inspection finished (read-only; see report scope/limits)":"Partial report - access fault";
        }
        catch(...){std::lock_guard<std::mutex> lock(s_mutex);s_report.clear();s_status="Inspection failed; repeat after loading";}
        s_busy=false;
    }
    void DrawPanel()
    {
        BasicAimTrace::DrawPanel();
        SilentAimExperiment::DrawPanel();
        AttackInspector::DrawPanel();
#if ALWAYSLOSE_HAS_MATERIAL_INSPECTOR
        MaterialInspector::DrawPanel();
#endif
        UITheme::BeginCard("sky_inspector",UITheme::Icon::Info,"Sky inspector - read only");
        UITheme::InfoText("Enter Conquest, then inspect once. This identifies sky/light candidates and material overrides; it does not change the sky.");
        const bool waiting=s_pending.load()||s_busy.load();
        ImGui::BeginDisabled(waiting);
        if(ImGui::Button("Inspect sky",{UITheme::Px(180.f),UITheme::Px(36.f)}))s_pending=true;
        ImGui::EndDisabled();
        std::string report,status;{std::lock_guard<std::mutex> lock(s_mutex);report=s_report;status=s_status;}
        ImGui::SameLine();ImGui::BeginDisabled(report.empty()||waiting);
        if(ImGui::Button("Copy sky report",{UITheme::Px(190.f),UITheme::Px(36.f)}))ImGui::SetClipboardText(report.c_str());
        ImGui::EndDisabled();
        ImGui::TextWrapped("Status: %s",waiting?"Waiting for window dispatcher / inspecting...":status.c_str());
        if(!report.empty()&&ImGui::TreeNode("Sky report preview"))
        {
            ImGui::BeginChild("sky_report_text",{0.f,UITheme::Px(260.f)},true);
            ImGui::TextUnformatted(report.c_str());ImGui::EndChild();ImGui::TreePop();
        }
        UITheme::InfoText("One-shot only. Copy explicitly; no automatic file export. Missing candidates do not prove that no sky exists.");
        UITheme::EndCard();
    }
}
