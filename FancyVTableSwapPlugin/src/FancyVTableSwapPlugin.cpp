#include "pch.h"
#include "FancyVTableSwapPlugin.h"
#include "ACU/basic_types.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "imgui/imgui.h"
#include <fstream>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>

// Null-safe type read. NEVER call action->GetEnumParkourAction() directly:
// type 52 tables can have a NULL fancy fn at idx 0x45 and
// GET_AND_CAST_FANCY_FUNC does no null check (see FailedWallRunPlugin.cpp).
static constexpr uint64 k_Type52FancyVTable_Runtime = 0x143A522F0; // 1.5.0 verified
static constexpr uint64 k_Type52FancyVTable_TransientBase = 0x143A41930;
static int GetTypeIntSafe(const AvailableParkourAction* action)
{
    if (!action || !action->fancyVTable) return -1;
    const FancyVFunction& slot = action->fancyVTable->fancyVirtuals[ParkourActionKnownFancyVFuncs::GetEnumParkourAction::idx];
    if (!slot.fn)
    {
        if ((uint64)action->fancyVTable == k_Type52FancyVTable_Runtime ||
            (uint64)action->fancyVTable == k_Type52FancyVTable_TransientBase)
            return (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
        return -1;
    }
    return (int)GET_AND_CAST_FANCY_FUNC(*action, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)((AvailableParkourAction*)action);
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
    // Single-purpose plugin: no ChooseAfterSorting force-select, so leave it null.
    // Priority before ACUFixes' enforcer (10000) so swaps happen before any
    // force-selection reads the enum/fitness from the new table.
    m_Callbacks.ChooseAfterSorting_fnp = nullptr;
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 4.0f;
    m_Callbacks.m_Name = "FancyVTableSwapPlugin_Swap";
    LoadSettings();
    m_HexDirty = true;
}

FancyVTableSwapPlugin::~FancyVTableSwapPlugin()
{
    if (m_Activator)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_Activator.reset();
    }
}

void FancyVTableSwapPlugin::OnBeforeActivate()
{
    auto gph = GenericHooksInParkourFiltering::GetSingleton();
    gph->Subscribe(m_Callbacks);
    m_Activator = gph->RequestGPHSortAndSelect();
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
            else if (line.rfind("SourceVTable=", 0) == 0)
                try { m_SourceVTable = std::stoull(line.substr(13), nullptr, 0); } catch (...) {}
            else if (line.rfind("DestVTable=", 0) == 0)
                try { m_DestVTable = std::stoull(line.substr(11), nullptr, 0); } catch (...) {}
            else if (line.rfind("TypeFilter=", 0) == 0)
                try { m_TypeFilter = std::stoi(line.substr(11)); } catch (...) {}
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
            file << "SourceVTable=" << std::hex << m_SourceVTable << std::dec << "\n";
            file << "DestVTable=" << std::hex << m_DestVTable << std::dec << "\n";
            file << "TypeFilter=" << m_TypeFilter << "\n";
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
    if (m_Enabled && !m_HoldMode && (GetAsyncKeyState(m_Hotkey) & 1))
    {
        m_ToggleState = !m_ToggleState;
        SaveSettings();
    }
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

    bool holdMode = m_HoldMode;
    if (ImGui::RadioButton("Hold", holdMode)) { m_HoldMode = true; SaveSettings(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Toggle", !holdMode)) { m_HoldMode = false; SaveSettings(); }

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
    ImGui::Text("Last cycle: %d actions, %d swapped", m_LastActionsSeen, m_LastSwapped);
    ImGui::Text("Rule: %s vtable 0x%llX -> 0x%llX",
        m_TypeFilter < 0 ? "any" : ("type " + std::to_string(m_TypeFilter)).c_str(),
        (unsigned long long)m_SourceVTable, (unsigned long long)m_DestVTable);

    // Discovered tables this cycle (live addresses, so no hardcoded table needed).
    std::vector<DiscoveredVTable> disc;
    {
        std::lock_guard lk(m_Mutex);
        disc = m_Discovered;
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
        ImGui::Text("No parkour cycle seen yet — move / face a wall to populate.");
    }
    ImGui::TextWrapped("Swapping changes the move's behaviour live (fitness, filters, enum). "
        "Only use tables of the same 0x78-entry size; mismatched tables can crash.");
}
