#include "auto_pick.h"
#include "../core/sdk_helpers.h"
#include "../core/hooks.h"
#include "../ui/theme.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <random>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace AutoPick
{
    struct GodLabel { std::string key, label; };
    static std::mutex s_mutex;
    static Config s_config;
    static std::vector<GodLabel> s_catalog;
    static std::string s_status = "Disabled", s_catalogStatus = "Waiting for lobby catalog";
    static std::atomic<bool> s_enabled{false}, s_stopping{false};
    static std::atomic<uint64_t> s_catalogUntil{0}, s_lastWindowTick{0};
    static std::atomic<unsigned> s_calls{0}, s_checks{0};
    static std::atomic<HWND> s_timerWindow{nullptr};
    static std::atomic<UINT> s_setupMessage{0};
    static int s_timerToken;
    static const UINT_PTR TimerId = reinterpret_cast<UINT_PTR>(&s_timerToken);
    static uint64_t s_lastPost = 0;
    // Window-thread-only state, containing values, never retained UObject pointers.
    static uint64_t s_nextCatalog = 0;
    static uintptr_t s_lastWorld = 0, s_lastDraft = 0;
    static int s_lastIndex = -1;
    static SDK::FName s_lastName{};
    static uint16_t s_lastTask = 0;
    static bool s_attempted = false;
    static thread_local bool s_inPoll = false;
    static std::mt19937 s_rng{static_cast<unsigned>(GetTickCount64())};

    Config GetConfig() { std::lock_guard<std::mutex> lock(s_mutex); return s_config; }
    void SetConfig(const Config& value)
    {
        Config next = value;
        next.Mode = next.Mode == 1 ? 1 : 0;
        next.TriggerSeconds = std::isfinite(next.TriggerSeconds)
            ? std::clamp(next.TriggerSeconds, 0.5f, 5.f) : 1.f;
        if (next.GodKey.size() > 128) next.GodKey.resize(128);
        std::lock_guard<std::mutex> lock(s_mutex);
        s_config = next; s_enabled = next.Enabled;
    }
    std::string Status() { std::lock_guard<std::mutex> lock(s_mutex); return s_status; }
    static void Publish(const char* text) { std::lock_guard<std::mutex> lock(s_mutex); s_status = text; }
    static void CatalogStatus(const char* text) { std::lock_guard<std::mutex> lock(s_mutex); s_catalogStatus = text; }
    static bool Live(SDK::UObject* object)
    {
        if (!object || !Helpers::IsValidPtr(object)) return false;
        const uint32_t invalid = static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject) |
            static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
            static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
            static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
        return !(static_cast<uint32_t>(object->Flags) & invalid);
    }
    static std::string LabelFor(SDK::UHWCharacterItem* item, const std::string& key)
    {
        const auto& text = item->ItemDisplayNameCAPS;
        if (text.TextData && Helpers::IsValidPtr(text.TextData))
        {
            auto label = text.ToString();
            if (!label.empty() && label.size() < 160) return label;
        }
        for (const char* prefix : {"CharacterItem_", "ZCharacter_", "CI_"})
            if (key.rfind(prefix, 0) == 0) return key.substr(std::char_traits<char>::length(prefix));
        return key;
    }
    static void AddGod(std::vector<GodLabel>& catalog, SDK::UHWCharacterItem* item)
    {
        if (!Live(item) || !item->IsA(SDK::UHWCharacterItem::StaticClass()) || item->bIsDisabled) return;
        const auto key = item->GetName();
        if (!key.empty()) catalog.push_back({key, LabelFor(item, key)});
    }
    static void AddGodSafe(std::vector<GodLabel>& catalog, SDK::UHWCharacterItem* item)
    {
        __try { AddGod(catalog, item); }
        __except (EXCEPTION_EXECUTE_HANDLER) {} // One bad entry must not block the entire roster.
    }
    static void StoreCatalog(std::vector<GodLabel> catalog, const char* source)
    {
        if (catalog.empty()) return; // Preserve last string-only catalog while a new lobby loads.
        std::sort(catalog.begin(), catalog.end(), [](const auto& a,const auto& b){return a.key < b.key;});
        catalog.erase(std::unique(catalog.begin(), catalog.end(), [](const auto& a,const auto& b){return a.key == b.key;}), catalog.end());
        std::sort(catalog.begin(), catalog.end(), [](const auto& a,const auto& b){return a.label < b.label;});
        char message[160]; std::snprintf(message,sizeof(message),"%zu gods (%s); availability checked in draft",catalog.size(),source);
        std::lock_guard<std::mutex> lock(s_mutex); s_catalog = std::move(catalog); s_catalogStatus = message;
    }
    static void RefreshLobbyCatalog(SDK::UWorld* world)
    {
        auto* raw = SDK::USubsystemBlueprintLibrary::GetGameInstanceSubsystem(world, SDK::UHWGodSubsystem::StaticClass());
        if (!Live(raw) || !raw->IsA(SDK::UHWGodSubsystem::StaticClass()))
            { CatalogStatus("Lobby god subsystem not available yet"); return; }
        auto* subsystem = static_cast<SDK::UHWGodSubsystem*>(raw);
        std::vector<GodLabel> catalog;
        auto collect = [&](const auto& items) {
            if (!items.IsValid() || items.Num() < 0 || items.Num() > 512) return;
            for (int i=0; i<items.Num(); ++i)
                if (items.IsValidIndex(i)) AddGodSafe(catalog, items[i]);
        };
        collect(subsystem->CachedPlayableGods);
        if (catalog.empty()) collect(subsystem->CachedActiveGods);
        if (catalog.empty()) collect(subsystem->FullListOfGods);
        if (catalog.empty()) CatalogStatus("Lobby catalog is still loading; retrying once per second");
        else StoreCatalog(std::move(catalog), "lobby catalog");
    }
    static bool IsSelectable(const SDK::FDraftChoice& choice)
    {
        return choice.ChoiceObject && choice.bIsVisibleToUI && !choice.bIsUnavailable &&
            !choice.bIsUnavailableByOwnership && !choice.bIsBanned &&
            !choice.bAlreadyPicked && !choice.bAlreadyLocked && !choice.bPendingCompleteBySelf;
    }
    static bool IsPickTask(const SDK::FDraftTask& task)
    {
        return SDK::ADraftReplicatedActor::IsTaskValid(task) &&
            (task.TaskType == SDK::EDraftTaskType::Pick || task.TaskType == SDK::EDraftTaskType::PickAndLock);
    }
    static void CheckDraft(SDK::AHWDraftReplicatedActor* draft, SDK::UWorld* world)
    {
        auto task = draft->LocalPersonalActiveTask;
        if (!SDK::ADraftReplicatedActor::IsTaskValid(task)) task = draft->PersonalActiveTask;
        if (!IsPickTask(task)) { Publish("Your active task is not a god pick (ban/lock/wait)"); return; }
        const uintptr_t worldKey = reinterpret_cast<uintptr_t>(world), draftKey = reinterpret_cast<uintptr_t>(draft);
        if (worldKey != s_lastWorld || draftKey != s_lastDraft || draft->Index != s_lastIndex || draft->Name != s_lastName || task.TaskId.ID != s_lastTask)
        {
            s_lastWorld=worldKey; s_lastDraft=draftKey; s_lastIndex=draft->Index;
            s_lastName=draft->Name; s_lastTask=task.TaskId.ID; s_attempted=false;
        }
        const Config options = GetConfig();
        const auto& choices = draft->SortedChoices.Num() ? draft->SortedChoices : draft->Choices;
        if (!choices.IsValid() || choices.Num()<0 || choices.Num()>512) { Publish("Invalid draft choices; skipped"); return; }
        bool selected = task.SelectedChoice || draft->PersonalActiveTask.SelectedChoice || draft->bLocalPersonalTaskPendingComplete;
        for (int i=0;i<choices.Num();++i)
        {
            if (!choices.IsValidIndex(i)) continue;
            const auto& choice=choices[i];
            selected = selected || choice.bSelectedBySelf || choice.bLockedByLocalPlayer || choice.bPendingCompleteBySelf;
        }
        if (selected) { Publish("Existing choice kept; no Lock In"); return; }
        if (s_attempted) return;
        if (draft->GetTimerStateByTaskId(task.TaskId) != SDK::EPGame_ReplicateTimerState::Active)
            { Publish("Waiting for an active personal pick timer"); return; }
        const float remaining = draft->GetTimeRemainingByTaskId(task.TaskId);
        if (!std::isfinite(remaining) || remaining<=0.f) { Publish("Personal pick timer has expired or is unavailable"); return; }
        if (remaining>options.TriggerSeconds)
        {
            char text[160];std::snprintf(text,sizeof(text),"Personal pick: %.1f s left; selecting at %.1f s",remaining,options.TriggerSeconds);
            Publish(text); return;
        }
        std::vector<SDK::UObject*> available;
        for (int i=0;i<choices.Num();++i)
        {
            if (!choices.IsValidIndex(i)) continue;
            const auto& choice=choices[i]; auto* object=choice.ChoiceObject;
            if (!IsSelectable(choice) || !Live(object) || !object->IsA(SDK::UHWCharacterItem::StaticClass())) continue;
            auto* god=static_cast<SDK::UHWCharacterItem*>(object);
            if (!god->bIsDisabled && draft->IsValidChoiceForPersonalActiveTask(object)==SDK::ECanCompleteDraftTaskResult::Ok)
                available.push_back(object);
        }
        if (available.empty()) { Publish("No selectable gods in your personal task"); return; }
        SDK::UObject* choice=nullptr;
        if (options.Mode==1)
        {
            for (auto* object:available) if (object->GetName()==options.GodKey) {choice=object;break;}
            if (!choice) {Publish("Specific god is unavailable; no random fallback");return;}
        }
        else choice=available[std::uniform_int_distribution<size_t>(0,available.size()-1)(s_rng)];
        s_attempted=true;
        const auto result=draft->RequestSelect(choice);
        Publish(result==SDK::ECanCompleteDraftTaskResult::Ok
            ? "Selection request sent; Lock In was NOT pressed" : "Selection rejected; no retry or Lock In");
    }
    static void Poll()
    {
        const Config options=GetConfig();
        auto* world=Helpers::GetWorld();
        if (!Live(world)) {Publish("Waiting for a world / lobby");return;}
        const uint64_t now=GetTickCount64();
        if (now<s_catalogUntil.load() && now>=s_nextCatalog)
        {
            s_nextCatalog=now+1000;
            RefreshLobbyCatalog(world);
        }
        if (!options.Enabled) {Publish("Disabled; lobby catalog can still be loaded");return;}
        auto* pc=Helpers::GetLocalPC();
        if (!Live(pc) || !pc->IsA(SDK::AHWPlayerController::StaticClass())) {Publish("Waiting for local SMITE player controller");return;}
        auto* hwpc=static_cast<SDK::AHWPlayerController*>(pc);
        // SDK const TMap::operator[] discards const inside TSparseArray.
        // Use its non-const accessor for read-only traversal; do not copy/mutate the map.
        auto& drafts=hwpc->DraftActors;
        if (!drafts.IsValid() || drafts.Num()<0 || drafts.Num()>32 || drafts.NumAllocated()>128)
            {Publish("Local draft map unavailable");return;}
        for (int i=0;i<drafts.NumAllocated();++i)
        {
            if (!drafts.IsValidIndex(i)) continue;
            auto* draft=drafts[i].Value().Get();
            if (!Live(draft) || !draft->IsA(SDK::AHWDraftReplicatedActor::StaticClass())) continue;
            if (draft->OwningPlayerController!=pc || draft->bDraftHasEnded || draft->bLocalAllPersonalTasksAreComplete) continue;
            auto* hwDraft=static_cast<SDK::AHWDraftReplicatedActor*>(draft);
            if (hwDraft->GetDraftType()==SDK::EHWDraftType::Skin) continue;
            CheckDraft(hwDraft,world); return;
        }
        Publish("No active personal god draft (lobby, match or transition)");
    }
    static void PollSafe()
    {
        __try { Poll(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Publish("SDK access failed; skipped this check"); }
    }
    struct ActiveCall { ActiveCall(){++s_calls;} ~ActiveCall(){--s_calls;} };
    bool HandleWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM)
    {
        const UINT setup=s_setupMessage.load();
        const bool isSetup=setup && message==setup;
        const bool isTimer=message==WM_TIMER && wParam==TimerId;
        if (!isSetup && !isTimer) return false;
        ActiveCall active;
        if (s_stopping.load()) return true;
        if (!hwnd || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId()) return true;
        const uint64_t now=GetTickCount64();
        const bool needed=s_enabled.load() || now<s_catalogUntil.load();
        if (!needed)
        {
            KillTimer(hwnd,TimerId); s_timerWindow=nullptr; return true;
        }
        if (isSetup)
        {
            HWND old=s_timerWindow.load(); if (old && old!=hwnd) KillTimer(old,TimerId);
            if (SetTimer(hwnd,TimerId,100,nullptr))
            {
                s_timerWindow=hwnd;
                if (!s_checks.load()) Publish("Window timer ready; waiting for first draft check");
            }
            else Publish("Could not start window draft timer");
            return true;
        }
        if (s_inPoll) return true;
        s_inPoll=true; s_lastWindowTick=now; ++s_checks; PollSafe(); s_inPoll=false;
        return true;
    }
    void Service()
    {
        if (s_stopping.load()) return;
        const uint64_t now=GetTickCount64();
        if (!s_enabled.load() && now>=s_catalogUntil.load()) return;
        HWND hwnd=Hooks::g_hWnd;
        if (!hwnd || !IsWindow(hwnd)) {Publish("Waiting for game window");return;}
        if (!s_setupMessage.load())
        {
            const UINT id=RegisterWindowMessageW(L"ALWAYSLOSE.AutoPick.WindowTimer.v2");
            if (!id) {Publish("Window dispatcher registration failed");return;}
            s_setupMessage=id;
        }
        const bool ready=s_timerWindow.load()==hwnd && now-s_lastWindowTick.load()<2000;
        if (ready || now-s_lastPost<1000) return;
        s_lastPost=now;
        if (!PostMessageW(hwnd,s_setupMessage.load(),0,0)) Publish("Could not post draft timer setup");
        else Publish("Waiting for game-window dispatcher (Auto Pick v2)");
    }
    void Shutdown()
    {
        s_stopping=true;
        HWND hwnd=s_timerWindow.exchange(nullptr);
        if (hwnd) KillTimer(hwnd,TimerId);
        while (s_calls.load()) Sleep(1);
    }
    void DrawSettings()
    {
        s_catalogUntil=GetTickCount64()+2000;
        Config options=GetConfig();
        UITheme::BeginCard("cfg_auto_pick",UITheme::Icon::Target,"Auto Pick (no Lock In)",nullptr,440.f);
        bool changed=UITheme::CheckboxRow("Enabled",&options.Enabled);
        UITheme::InfoText("Select near the end of your personal pick timer. Existing selections are kept.");
        ImGui::Spacing();
        const char* modes[]={"Random available god","Specific god"};
        changed|=UITheme::ComboRow("Selection mode",&options.Mode,modes,2);
        std::vector<GodLabel> catalog;std::string catalogStatus;
        {std::lock_guard<std::mutex> lock(s_mutex);catalog=s_catalog;catalogStatus=s_catalogStatus;}
        if (options.Mode==1)
        {
            const char* preview=options.GodKey.empty()?"Choose a god":options.GodKey.c_str();
            for (const auto& item:catalog) if (item.key==options.GodKey) preview=item.label.c_str();
            ImGui::TextWrapped("God (can be chosen in the lobby)");UITheme::HelpLastItem("Preferred god","Choose a catalog entry in advance. Availability is checked in the active draft; unavailable gods are skipped without a random fallback.");ImGui::SetNextItemWidth(-1.f);
            if (ImGui::BeginCombo("##auto_pick_god",preview))
            {
                for (const auto& item:catalog)
                {
                    ImGui::PushID(item.key.c_str());
                    if (ImGui::Selectable(item.label.c_str(),item.key==options.GodKey)) {options.GodKey=item.key;changed=true;}
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            UITheme::HelpLastItem("Preferred god","Choose a catalog entry in advance. Availability is checked in the active draft; unavailable gods are skipped without a random fallback.");
        }
        changed|=UITheme::SliderFloatRow("Select when time left is at most",&options.TriggerSeconds,0.5f,5.f,"%.1f s");
        ImGui::TextWrapped("Catalog: %s",catalogStatus.c_str());
        if (UITheme::ActionButton("refresh_auto_pick_gods",UITheme::Icon::Reset,"Refresh god list",UITheme::palette.accent,{180.f,32.f}))
            s_catalogUntil=GetTickCount64()+5000;
        if (changed) SetConfig(options);
        UITheme::InfoText("Only selects. No Lock In. Existing choices are kept; unavailable gods are skipped.");
        ImGui::TextWrapped("Status: %s",Status().c_str());
        ImGui::Text("Window checks: %u",s_checks.load());
        UITheme::EndCard();
    }
}
