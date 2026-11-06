#include "pch.h"
#include "FancyVTableSwapPlugin.h"
#include "ACU/basic_types.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/ParkourLog.h"
#include "ParkourDebugging/GetTypeIntSafe.h"
#include "imgui/imgui.h"
#include <fstream>
#include <shlobj.h>
#include <unordered_map>
#include <algorithm>
#include <cstdio>

// Static enforcer flag, consulted by the ParkourLog weights hook and the
// fitness-cull skip. Refreshed on the game thread by the plugin callbacks.
static bool s_EnforceActive = false;
bool FVTS_IsEnforcing()
{
    return s_EnforceActive;
}
// Skip the ~zero-fitness cull for type-52 candidates while the enforcer is
// armed: a failed wallrun's fitness is ~0 by design, and the cull would erase
// every enforced candidate before the (later) weight boost or force-select.
bool FVTS_ShouldSkipFitnessCull(AvailableParkourAction* action)
{
    return s_EnforceActive
        && action
        && GetTypeIntSafe(action) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
}

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\FancyVTableSwapPlugin.ini";
    return "FancyVTableSwapPlugin.ini";
}

FancyVTableSwapPlugin::FancyVTableSwapPlugin()
{
    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction* {
        return static_cast<FancyVTableSwapPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.ChooseAfterSorting_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame) -> AvailableParkourAction* {
        return static_cast<FancyVTableSwapPlugin*>(userData)->ChooseAfterSorting(actions, selectedByGame);
    };
    m_Callbacks.OverrideCreationDiscard_fnp = [](void* userData, AvailableParkourAction& action) -> bool {
        return static_cast<FancyVTableSwapPlugin*>(userData)->OnCreationDiscardOverride(action);
    };
    // Priority before ACUFixes' enforcer (10000).
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 4.0f;
    m_Callbacks.m_Name = "FancyVTableSwapPlugin_Enforce52";
    LoadSettings();
}

FancyVTableSwapPlugin::~FancyVTableSwapPlugin()
{
    if (m_Activator || m_ActivatorCreation)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_Activator.reset();
        m_ActivatorCreation.reset();
    }
}

void FancyVTableSwapPlugin::OnBeforeActivate()
{
    auto gph = GenericHooksInParkourFiltering::GetSingleton();
    gph->Subscribe(m_Callbacks);
    m_Activator = gph->RequestGPHSortAndSelect();
    m_ActivatorCreation = gph->RequestGPHCreation();
}

bool FancyVTableSwapPlugin::IsActive() const
{
    if (!m_Enabled) return false;
    if (m_HoldMode) return (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
    return m_ToggleState;
}

void FancyVTableSwapPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        try
        {
            if (line.rfind("Hotkey=", 0) == 0)
                try { m_Hotkey = std::stoi(line.substr(7), nullptr, 16); } catch (...) {}
            else if (line.rfind("Enabled=", 0) == 0)
                m_Enabled = (line.substr(8) == "1");
            else if (line.rfind("HoldMode=", 0) == 0)
                try { m_HoldMode = std::stoi(line.substr(9)) != 0; } catch (...) {}
            else if (line.rfind("ToggleState=", 0) == 0)
                try { m_ToggleState = std::stoi(line.substr(12)) != 0; } catch (...) {}
        }
        catch (...) {}
    }
    if (m_Hotkey < 1 || m_Hotkey > 0xFE) m_Hotkey = 0x77;
}

void FancyVTableSwapPlugin::SaveSettings() const
{
    try
    {
        std::ofstream file(GetIniPath());
        if (file)
        {
            file << "Hotkey=" << std::hex << m_Hotkey << std::dec << "\n";
            file << "Enabled=" << (m_Enabled ? 1 : 0) << "\n";
            file << "HoldMode=" << (m_HoldMode ? 1 : 0) << "\n";
            file << "ToggleState=" << (m_ToggleState ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

// Diagnostics only (no mutations): refresh the (vtable, type) snapshot.
AvailableParkourAction* FancyVTableSwapPlugin::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    s_EnforceActive = m_Enabled && IsActive();
    std::vector<DiscoveredVTable> found;
    found.reserve(actions.size);

    for (AvailableParkourAction* action : actions)
    {
        if (!action || !action->fancyVTable) continue;
        uint64 vtable = (uint64)action->fancyVTable;
        int type = GetTypeIntSafe(action);
        auto it = std::find_if(found.begin(), found.end(),
            [&](const DiscoveredVTable& d) { return d.vtable == vtable && d.type == type; });
        if (it != found.end()) it->count++;
        else found.push_back({ vtable, type, 1 });
    }

    std::lock_guard lk(m_Mutex);
    m_Discovered = std::move(found);
    m_LastActionsSeen = (int)actions.size;
    m_ChooseFired = true;
    return nullptr; // diagnostics only; never force-select
}

// Enforce-52 stage 1 (discard override): keep natural type-52s alive even
// when the vanilla verdict rejects them — on handhold walls the freeform
// wallrun gets discarded here, which is exactly what we waive. NO vtable
// swapping: a converted 53 runs with 53-computed trajectory data and behaves
// unpredictably (unexplained height, ground clipping). The freeform wallrun
// must be the one the game created itself, like the proven CheatEngine
// approach: fitness boost + keep-alive + selection, never a type rewrite.
bool FancyVTableSwapPlugin::OnCreationDiscardOverride(AvailableParkourAction& action)
{
    s_EnforceActive = m_Enabled && IsActive();
    if (!s_EnforceActive) return false;
    if (GetTypeIntSafe(&action) != (int)EnumParkourAction::wallrunUpFromGroundFailed_mb) return false;
    std::lock_guard lk(m_Mutex);
    ++m_EnforceKeptAlive;
    return true;
}

// Enforce-52 stage 3 (force-select): prefer a type-52 that passed both final
// filters; fall back to the first 52 if none passed (ACUFixes' own enforcer
// also forces unfiltered picks). Stages 1 (keep at creation) and 2 (weight
// boost in the ParkourLog weights hook) get at least one 52 into the sorted
// array even on handhold walls.
AvailableParkourAction* FancyVTableSwapPlugin::ChooseAfterSorting(
    SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame)
{
    (void)selectedByGame;
    s_EnforceActive = m_Enabled && IsActive();
    if (!s_EnforceActive) return nullptr;

    // Final-filter verdicts for THIS cycle's actions, from the scanner.
    auto cycle = ParkourLog::GetSingleton().GetCurrentLoggedParkourCycle();
    std::unordered_map<AvailableParkourAction*, std::pair<bool, bool>> filterResults;
    if (cycle)
    {
        std::lock_guard cycleLock(cycle->m_Mutex);
        for (auto& record : cycle->m_Actions)
        {
            if (!record) continue;
            filterResults.emplace(record->m_ActionPtr,
                std::make_pair(record->m_ResultOfFinalFilter1.value_or(true),
                               record->m_ResultOfFinalFilter2.value_or(true)));
        }
    }
    auto IsPassedBoth = [&](AvailableParkourAction* action) {
        auto it = filterResults.find(action);
        return it != filterResults.end() && it->second.first && it->second.second;
    };

    AvailableParkourAction* first52 = nullptr;
    for (AvailableParkourAction* action : actions)
    {
        if (!action) continue;
        if (GetTypeIntSafe(action) != (int)EnumParkourAction::wallrunUpFromGroundFailed_mb) continue;
        if (!first52)
        {
            std::lock_guard lk(m_Mutex);
            first52 = action;
            m_EnforceLastSelectedVTable = (uint64)action->fancyVTable;
        }
        if (IsPassedBoth(action))
        {
            std::lock_guard lk(m_Mutex);
            m_EnforceLastSelectedType = 52;
            m_EnforceLastSelectedVTable = (uint64)action->fancyVTable;
            return action;
        }
    }
    {
        std::lock_guard lk(m_Mutex);
        m_EnforceLastSelectedType = first52 ? 52 : -1;
        return first52; // nullptr when no 52 survived into the sorted array
    }
}

void FancyVTableSwapPlugin::OnUpdate()
{
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (GetAsyncKeyState(vk) & 1)
            {
                m_Hotkey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
        return;
    }
    s_EnforceActive = m_Enabled && IsActive();
    if (m_Enabled && !m_HoldMode && (GetAsyncKeyState(m_Hotkey) & 1))
    {
        m_ToggleState = !m_ToggleState;
        SaveSettings();
    }
}

void FancyVTableSwapPlugin::OnImGuiRender()
{
    bool enabled = m_Enabled;
    if (ImGui::Checkbox("Enable Failed Wallrun Enforcer (wallrunUpFromGroundFailed while hotkey ACTIVE)", &enabled))
    {
        m_Enabled = enabled;
        SaveSettings();
    }
    if (!m_Enabled) return;

    ImGui::Text("Status: %s", IsActive() ? "ACTIVE" : "INACTIVE");

    bool holdMode = m_HoldMode;
    if (ImGui::RadioButton("Hold", holdMode)) { m_HoldMode = true; SaveSettings(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Toggle", !holdMode)) { m_HoldMode = false; SaveSettings(); }

    ImGui::Text("Hotkey: 0x%02X", m_Hotkey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey ? "Cancel" : "Rebind"))
        m_WaitingForKey = !m_WaitingForKey;
    if (m_WaitingForKey) ImGui::Text("Press any key...");

    ImGui::Separator();
    ImGui::Text("Native 52s kept alive: %d this session", m_EnforceKeptAlive);
    if (m_EnforceLastSelectedType == 52)
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "  Last sort: selected type 52 (vtable 0x%llX)",
            (unsigned long long)m_EnforceLastSelectedVTable);
    else if (m_EnforceLastSelectedType >= 0)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1.0f), "  Last sort: selected type %d — no 52 won", m_EnforceLastSelectedType);
    else
        ImGui::Text("  Last sort: no selection this cycle");

    ImGui::Text("Last filtered cycle (sort&select hook): %d actions", m_LastActionsSeen);

    // Scanner diagnostics: actions logged at creation.
    int scannedActions = 0;
    {
        auto cycle = ParkourLog::GetSingleton().GetLatestLoggedParkourCycle();
        if (cycle)
        {
            std::lock_guard cycleLock(cycle->m_Mutex);
            for (auto& record : cycle->m_Actions)
                if (record && !record->m_IsDiscarded_immediatelyAfterCreation) ++scannedActions;
        }
    }
    ImGui::Text("Scanner (creation hook): %d live actions, last cycle", scannedActions);

    // Discovered tables (scanner fallback if the sort hook never fires).
    std::vector<DiscoveredVTable> disc;
    {
        std::lock_guard lk(m_Mutex);
        disc = m_Discovered;
    }
    if (disc.empty())
    {
        auto cycle = ParkourLog::GetSingleton().GetLatestLoggedParkourCycle();
        if (cycle)
        {
            std::lock_guard cycleLock(cycle->m_Mutex);
            for (auto& record : cycle->m_Actions)
            {
                if (!record || record->m_IsDiscarded_immediatelyAfterCreation) continue;
                auto it = std::find_if(disc.begin(), disc.end(), [&](const DiscoveredVTable& d) {
                    return d.vtable == record->m_FancyVTable;
                });
                if (it != disc.end()) it->count++;
                else disc.push_back({ record->m_FancyVTable, (int)record->m_ActionType, 1 });
            }
        }
    }
    if (!disc.empty())
    {
        ImGui::Text("Discovered last cycle (%zu):", disc.size());
        for (const DiscoveredVTable& d : disc)
            ImGui::Text("  0x%llX type %d x%d",
                (unsigned long long)d.vtable, d.type, d.count);
    }
    else
    {
        ImGui::Text("No parkour cycle seen yet — with you on a wall this means the scanner hooks aren't logging. Confirm game version is 1.5.0.");
    }
    ImGui::TextWrapped("Hold/toggle the hotkey while running at any wall: naturally-created freeform wallrun (52) "
        "candidates are kept alive and weight-boosted, then forced through selection over the normal "
        "wallrun-up (53) and wallrun-to-overhang. No vtable swapping — the move keeps its own data.");
}
