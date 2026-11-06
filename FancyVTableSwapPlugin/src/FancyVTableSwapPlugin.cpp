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
#include <algorithm>
#include <cstdio>

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\FancyVTableSwapPlugin.ini";
    return "FancyVTableSwapPlugin.ini";
}

// Static enforcer flag consulted by the ParkourLog weights hook. Set on the
// game thread once per cycle-frame; a torn read would at most flip one cycle.
static bool s_EnforceActive = false;
bool FVTS_IsEnforcing()
{
    return s_EnforceActive;
}
// Skip the ~zero-fitness cull for type-52 candidates while the enforcer is
// armed: their fitness is ~0 by design, and they're removed before the
// weight boost could save them.
bool FVTS_ShouldSkipFitnessCull(AvailableParkourAction* action)
{
    return s_EnforceActive
        && action
        && GetTypeIntSafe(action) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
}

FancyVTableSwapPlugin::FancyVTableSwapPlugin()
{
    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction* {
        return static_cast<FancyVTableSwapPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    // Force-select for the failed-wallrun enforcer.
    m_Callbacks.ChooseAfterSorting_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame) -> AvailableParkourAction* {
        return static_cast<FancyVTableSwapPlugin*>(userData)->ChooseAfterSorting(actions, selectedByGame);
    };
    // Keep-alive + 53->52 conversion at creation (failed-wallrun enforcer).
    m_Callbacks.OverrideCreationDiscard_fnp = [](void* userData, AvailableParkourAction& action) -> bool {
        return static_cast<FancyVTableSwapPlugin*>(userData)->OnCreationDiscardOverride(action);
    };
    // Convert 53->52 right after construction, before type-specific setup.
    m_Callbacks.PostConstruct_fnp = [](void* userData, AvailableParkourAction& action) -> void {
        static_cast<FancyVTableSwapPlugin*>(userData)->OnPostConstruct(action);
    };
    m_Callbacks.m_UserData = this;
    // Priority before ACUFixes' enforcer (10000) so swaps happen before any
    // force-selection reads the enum/fitness from the new table.
    m_Callbacks.m_CallbackPriority = 4.0f;
    m_Callbacks.m_Name = "FancyVTableSwapPlugin_Swap";
    LoadSettings();
    m_HexDirty = true;
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
    // Creation-stage hooks feed the (now real) ParkourLog scanner — without
    // these the press-swap has nothing to scan.
    m_ActivatorCreation = gph->RequestGPHCreation();
}

// Press mode: scan the latest logged cycle and rewrite the matched actions'
// fancyVTable right now (press again to restore the creation-time originals).
// shortcut: records point to actions that get freed when their cycle ends —
// press while mid-parkour (e.g. hanging on a wall) to write live objects only.
void FancyVTableSwapPlugin::PressSwapNow()
{
    auto cycle = ParkourLog::GetSingleton().GetLatestLoggedParkourCycle();
    std::lock_guard lk(m_Mutex);
    if (m_PressIsOn)
    {
        int reverted = 0;
        for (const LiveSwap& swap : m_PressSwapsOn)
        {
            if (!swap.ptr) continue;
            swap.ptr->fancyVTable = (ParkourAction_FancyVTable*)swap.origVTable;
            ++reverted;
        }
        m_PressSwapsOn.clear();
        m_PressIsOn = false;
        m_PressLastMatched = reverted;
        m_PressLastSwapped = -reverted; // negative = reverted, for the UI line
        return;
    }
    if (!cycle)
    {
        m_PressLastMatched = 0;
        m_PressLastSwapped = 0;
        return;
    }
    const bool ruleValid = m_DestVTable != 0 && (m_SourceVTable != 0 || m_TypeFilter >= 0);
    if (!ruleValid)
    {
        m_PressLastMatched = 0;
        m_PressLastSwapped = 0;
        return;
    }
    int matched = 0;
    m_PressSwapsOn.clear();
    std::lock_guard cycleLock(cycle->m_Mutex);
    for (auto& record : cycle->m_Actions)
    {
        if (!record || record->m_IsDiscarded_immediatelyAfterCreation) continue;
        AvailableParkourAction* action = record->m_ActionPtr;
        if (!action || !action->fancyVTable) continue;
        bool matchSrc = (m_SourceVTable == 0 || record->m_FancyVTable == m_SourceVTable);
        bool matchType = (m_TypeFilter < 0 || (int)record->m_ActionType == m_TypeFilter);
        if (!matchSrc || !matchType) continue;
        if ((uint64)action->fancyVTable == m_DestVTable) continue;
        // The swap: one pointer write at +0x8. m_FancyVTable is the
        // creation-time original — the value to restore on revert.
        action->fancyVTable = (ParkourAction_FancyVTable*)m_DestVTable;
        m_PressSwapsOn.push_back({ action, record->m_FancyVTable });
        ++matched;
    }
    m_PressLastMatched = matched;
    m_PressLastSwapped = matched;
    m_PressIsOn = matched > 0;
}

bool FancyVTableSwapPlugin::IsActive() const
{
    if (!m_Enabled) return false;
    if (m_SwapMode == Mode_Hold) return (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
    if (m_SwapMode == Mode_Press) return m_PressIsOn;
    return m_ToggleState;
}

// Enforce-52 stage 0 (post-construct): while armed, convert every created
// wallrunUpFromGround_mb (53) into wallrunUpFromGroundFailed_mb (52) RIGHT
// AFTER construction, so all following creation steps (Set2Floats, InitialTest,
// fitness) run with the 52 table. Converting only after the initial test
// crashed: type-52 functions walked type-53-initialized data.
void FancyVTableSwapPlugin::OnPostConstruct(AvailableParkourAction& action)
{
    s_EnforceActive = m_Enabled && m_Enforce52 && IsActive();
    if (!s_EnforceActive) return;
    if (GetTypeIntSafe(&action) != (int)EnumParkourAction::wallrunUpFromGround_mb) return;
    // Same one-pointer write as the generic swap, targeting the verified
    // type-52 runtime table (same constants as FailedWallRunPlugin.cpp
    // uses — 0x143A522F0, 1.5.0).
    action.fancyVTable = (ParkourAction_FancyVTable*)k_Type52FancyVTable_Runtime;
    std::lock_guard lk(m_Mutex);
    ++m_EnforceConvertedThisCycle;
}

// Enforce-52 stage 1 (discard override): after the initial test, keep every
// already-converted (or naturally created) type-52 even when the vanilla
// verdict rejects it — the "no-handholds-in-range" context is exactly what
// this override waives on handhold walls.
// shortcut: return true unconditionally for 52 while armed (keeps also
// already-fitting 52s — harmless, they'd be kept anyway).
bool FancyVTableSwapPlugin::OnCreationDiscardOverride(AvailableParkourAction& action)
{
    s_EnforceActive = m_Enabled && m_Enforce52 && IsActive();
    if (!s_EnforceActive) return false;
    return GetTypeIntSafe(&action) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
}

// Enforce-52 stage 3 (force-select): first type-52 in the sorted array wins;
// stages 2 (weight boost, in ParkourLog weights logging) guarantees one
// survives the "fitness too low" removal even on handhold walls.
AvailableParkourAction* FancyVTableSwapPlugin::ChooseAfterSorting(
    SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame)
{
    (void)selectedByGame;
    s_EnforceActive = m_Enabled && m_Enforce52 && IsActive();
    if (!s_EnforceActive) return nullptr;
    for (AvailableParkourAction* action : actions)
    {
        if (!action) continue;
        if (GetTypeIntSafe(action) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb)
        {
            std::lock_guard lk(m_Mutex);
            m_EnforceLastSelectedType = 52;
            m_EnforceLastSelectedVTable = (uint64)action->fancyVTable;
            return action;
        }
    }
    std::lock_guard lk(m_Mutex);
    m_EnforceLastSelectedType = -1;
    m_EnforceLastSelectedVTable = 0;
    return nullptr;
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
            else if (line.rfind("SwapMode=", 0) == 0)
                try { m_SwapMode = std::stoi(line.substr(9)); } catch (...) {}
            else if (line.rfind("HoldMode=", 0) == 0) // legacy INI (press-mode didn't exist)
                try { m_SwapMode = (std::stoi(line.substr(9)) != 0) ? Mode_Hold : Mode_Toggle; } catch (...) {}
            else if (line.rfind("ToggleState=", 0) == 0)
                try { m_ToggleState = std::stoi(line.substr(12)) != 0; } catch (...) {}
            else if (line.rfind("SourceVTable=", 0) == 0)
                try { m_SourceVTable = std::stoull(line.substr(13), nullptr, 0); } catch (...) {}
            else if (line.rfind("DestVTable=", 0) == 0)
                try { m_DestVTable = std::stoull(line.substr(11), nullptr, 0); } catch (...) {}
            else if (line.rfind("TypeFilter=", 0) == 0)
                try { m_TypeFilter = std::stoi(line.substr(11)); } catch (...) {}
            else if (line.rfind("Enforce52=", 0) == 0)
                m_Enforce52 = (line.substr(10) == "1");
        }
        catch (...) {}
    }
    if (m_Hotkey < 1 || m_Hotkey > 0xFE) m_Hotkey = 0x77;
    if (m_SwapMode < Mode_Press || m_SwapMode > Mode_Toggle) m_SwapMode = Mode_Hold;
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
            file << "SwapMode=" << m_SwapMode << "\n";
            file << "ToggleState=" << (m_ToggleState ? 1 : 0) << "\n";
            file << "SourceVTable=" << std::hex << m_SourceVTable << std::dec << "\n";
            file << "DestVTable=" << std::hex << m_DestVTable << std::dec << "\n";
            file << "TypeFilter=" << m_TypeFilter << "\n";
            file << "Enforce52=" << (m_Enforce52 ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

void FancyVTableSwapPlugin::ApplyHexInputs()
{
    try { m_SourceVTable = std::stoull(m_SourceHex, nullptr, 0); } catch (...) {}
    try { m_DestVTable = std::stoull(m_DestHex, nullptr, 0); } catch (...) {}
}

AvailableParkourAction* FancyVTableSwapPlugin::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    // Always refresh the discovered (vtable, type) snapshot so the UI can
    // offer real game addresses even while the swap is off.
    std::vector<DiscoveredVTable> found;
    found.reserve(actions.size);
    int swapped = 0;
    const bool active = IsActive() && m_DestVTable != 0 &&
        (m_SourceVTable != 0 || m_TypeFilter >= 0);

    for (AvailableParkourAction* action : actions)
    {
        if (!action || !action->fancyVTable) continue;
        uint64 vtable = (uint64)action->fancyVTable;
        int type = GetTypeIntSafe(action);

        auto it = std::find_if(found.begin(), found.end(),
            [&](const DiscoveredVTable& d) { return d.vtable == vtable && d.type == type; });
        if (it != found.end()) it->count++;
        else found.push_back({ vtable, type, 1 });

        if (!active) continue;
        // shortcut: single rule only (one source -> one dest). For multiple
        // rules, upgrade to a vector of rules; not needed for v1.
        bool matchSrc = (m_SourceVTable == 0 || vtable == m_SourceVTable);
        bool matchType = (m_TypeFilter < 0 || type == m_TypeFilter);
        if (!matchSrc || !matchType) continue;
        if (vtable == m_DestVTable) continue;
        // The swap itself: one pointer write, same as ACUFixes 1.5.0
        // WhenStartingWallingCycle_ChangeFVTables (fancyVTable is at +0x8).
        action->fancyVTable = (ParkourAction_FancyVTable*)m_DestVTable;
        ++swapped;
    }

    {
        std::lock_guard lk(m_Mutex);
        m_Discovered = std::move(found);
        m_LastActionsSeen = (int)actions.size;
        m_LastSwapped = swapped;
        m_ChooseFired = true;
    }
    return nullptr; // mutate only; never force-select
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
    if (m_Enabled && m_SwapMode == Mode_Toggle && (GetAsyncKeyState(m_Hotkey) & 1))
    {
        m_ToggleState = !m_ToggleState;
        SaveSettings();
    }
    if (m_Enabled && m_SwapMode == Mode_Press && (GetAsyncKeyState(m_Hotkey) & 1))
        PressSwapNow();
}

void FancyVTableSwapPlugin::OnImGuiRender()
{
    bool enabled = m_Enabled;
    if (ImGui::Checkbox("Enable Fancy VTable Swapper", &enabled))
    {
        m_Enabled = enabled;
        SaveSettings();
    }
    if (!m_Enabled) return;

    const bool isActive = IsActive();
    ImGui::Text("Status: %s", isActive ? "ACTIVE" : "INACTIVE");

    bool holdMode = (m_SwapMode == Mode_Hold);
    if (ImGui::RadioButton("Press", m_SwapMode == Mode_Press)) { m_SwapMode = Mode_Press; SaveSettings(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Hold", holdMode)) { m_SwapMode = Mode_Hold; SaveSettings(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Toggle", m_SwapMode == Mode_Toggle)) { m_SwapMode = Mode_Toggle; SaveSettings(); }
    if (m_SwapMode == Mode_Press)
        ImGui::TextWrapped("Press hotkey: swaps matched actions of the last parkour cycle NOW. Press again: reverts to originals.");

    ImGui::Text("Hotkey: 0x%02X", m_Hotkey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey ? "Cancel" : "Rebind"))
        m_WaitingForKey = !m_WaitingForKey;
    if (m_WaitingForKey) ImGui::Text("Press any key...");

    if (m_HexDirty)
    {
        snprintf(m_SourceHex, sizeof(m_SourceHex), "0x%llX", (unsigned long long)m_SourceVTable);
        snprintf(m_DestHex, sizeof(m_DestHex), "0x%llX", (unsigned long long)m_DestVTable);
        m_HexDirty = false;
    }
    ImGui::InputText("Source vtable (0=any)", m_SourceHex, sizeof(m_SourceHex));
    ImGui::InputText("Dest vtable (0=off)", m_DestHex, sizeof(m_DestHex));
    if (ImGui::Button("Apply hex"))
    {
        ApplyHexInputs();
        SaveSettings();
    }

    // Type filter combo: -1 = any, else EnumParkourAction value.
    char typeFilterBuf[32];
    snprintf(typeFilterBuf, sizeof(typeFilterBuf), "type %d", m_TypeFilter);
    if (ImGui::BeginCombo("Type filter", m_TypeFilter < 0 ? "Any (vtable only)" : typeFilterBuf))
    {
        if (ImGui::Selectable("Any (vtable only)", m_TypeFilter < 0))
        {
            m_TypeFilter = -1;
            SaveSettings();
        }
        for (int i = 0; i < 125; ++i)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "type %d", i);
            if (ImGui::Selectable(buf, m_TypeFilter == i))
            {
                m_TypeFilter = i;
                SaveSettings();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    ImGui::Text("Last cycle (sort&select hook): %d actions, %d swapped",
        m_LastActionsSeen, m_LastSwapped);

    // Scanner diagnostics: if this line counts actions while the line above
    // stays at 0, actions ARE being scanned at creation but the sort&select
    // hook isn't running (e.g. another plugin owns the function start).
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
    if (m_SwapMode == Mode_Press)
    {
        std::lock_guard lk(m_Mutex);
        if (m_PressLastSwapped >= 0)
            ImGui::Text("Last press: %d matched, %d swapped%s",
                m_PressLastMatched, m_PressLastSwapped, m_PressIsOn ? " (press to revert)" : "");
        else
            ImGui::Text("Last press: reverted %d action(s)", m_PressLastMatched);
    }
    ImGui::Text("Rule: %s vtable 0x%llX -> 0x%llX",
        m_TypeFilter < 0 ? "any" : ("type " + std::to_string(m_TypeFilter)).c_str(),
        (unsigned long long)m_SourceVTable, (unsigned long long)m_DestVTable);

    // Discovered tables this cycle (live addresses, so no hardcoded table needed).
    // If the sort&select hook never fired, fall back to the creation scanner.
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
        ImGui::Text("Discovered this cycle (%zu):", disc.size());
        for (size_t i = 0; i < disc.size(); ++i)
        {
            char label[128];
            snprintf(label, sizeof(label), "0x%llX type %d x%d", (unsigned long long)disc[i].vtable, disc[i].type, disc[i].count);
            if (ImGui::Selectable(label))
            {
                m_SourceVTable = disc[i].vtable;
                if (disc[i].type >= 0) m_TypeFilter = disc[i].type;
                m_HexDirty = true;
                SaveSettings();
            }
            ImGui::SameLine();
            char btn[64];
            snprintf(btn, sizeof(btn), "-> Dest %zu", i);
            if (ImGui::Button(btn))
            {
                m_DestVTable = disc[i].vtable;
                m_HexDirty = true;
                SaveSettings();
            }
        }
    }
    else
    {
        ImGui::Text("No parkour cycle seen yet — with you on a wall this means the scanner hooks aren't logging. Confirm game version is 1.5.0.");
    }
    ImGui::Separator();

    // Failed-wallrun enforcer: hotkey-active -> created type 53 becomes type
    // 52, kept alive despite the discard, boosted to win the sort, and
    // force-selected. The one-goal path the generic rule can't reach, because
    // the killer is the reject-at-creation, not the vtable choice.
    if (ImGui::Checkbox("Enforce wallrunUpFromGroundFailed (52) while hotkey ACTIVE", &m_Enforce52))
        SaveSettings();
    if (m_Enforce52)
    {
        ImGui::Text("  Converted 53->52: %d this session", m_EnforceConvertedThisCycle);
        if (m_EnforceLastSelectedType == 52)
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "  Last sort: selected type 52 (vtable 0x%llX)",
                (unsigned long long)m_EnforceLastSelectedVTable);
        else if (m_EnforceLastSelectedType >= 0)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1.0f), "  Last sort: selected type %d — no 52 won", m_EnforceLastSelectedType);
        else
            ImGui::Text("  Last sort: no selection this cycle");
    }

    ImGui::TextWrapped("Swapping changes the move's behaviour live (fitness, filters, enum). "
        "Only use tables of the same 0x78-entry size; mismatched tables can crash.");
}
