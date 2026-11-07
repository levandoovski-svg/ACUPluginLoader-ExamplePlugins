#include "pch.h"
#include "FancyVTableSwapPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/GetTypeIntSafe.h"
#include "imgui/imgui.h"
#include <fstream>
#include <shlobj.h>
#include <cstdio>

// Passive freeform-wallrun plugin (the proven mode):
// while the hotkey is ACTIVE, the game's own wallrun candidates are discarded
// at creation through the vanilla reject path, and natural freeform 52s are
// kept alive. The game then picks among the surviving candidates itself.
//
// Deliberately NOT done (each was tested and/or chased across sessions):
//   - no sort&select site ownership: FreeJump's FinalFilter1 fallback cave
//     (per-candidate decide calls + wild calls on poisoned candidates) only
//     engages when another plugin machine-patches that site. Leaving the site
//     alone is what fixed the single-digit-FPS and the held-too-long crash.
//   - no 53->52 vtable conversion: a converted 53 runs with 53-computed
//     trajectory data and behaves unpredictably.
//   - no weight boost / force-select / fitness-cull edits.
//   - no teleport lift.

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\FancyVTableSwapPlugin.ini";
    return "FancyVTableSwapPlugin.ini";
}

FancyVTableSwapPlugin::FancyVTableSwapPlugin()
{
    m_Callbacks.OverrideCreationDiscard_fnp = [](void* userData, AvailableParkourAction& action) -> bool {
        return static_cast<FancyVTableSwapPlugin*>(userData)->OnCreationDiscardOverride(action);
    };
    m_Callbacks.ForceCreationDiscard_fnp = [](void* userData, AvailableParkourAction& action) -> bool {
        return static_cast<FancyVTableSwapPlugin*>(userData)->OnForceCreationDiscard(action);
    };
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 4.0f;
    m_Callbacks.m_Name = "FancyVTableSwapPlugin_DiscardWallruns";
    LoadSettings();
}

FancyVTableSwapPlugin::~FancyVTableSwapPlugin()
{
    if (m_ActivatorCreation)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_ActivatorCreation.reset();
    }
}

void FancyVTableSwapPlugin::OnBeforeActivate()
{
    GenericHooksInParkourFiltering::GetSingleton()->Subscribe(m_Callbacks);
    RefreshHookOwnership();
}

// Only the creation hooks are ours, ever. The sort&select site stays unpatched
// so FreeJump never engages its fallback cave (see file comment).
void FancyVTableSwapPlugin::RefreshHookOwnership()
{
    if (m_ActivatorCreation) return;
    m_ActivatorCreation = GenericHooksInParkourFiltering::GetSingleton()->RequestGPHCreation();
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

// Keep natural freeform 52s alive even when the vanilla verdict rejects them
// (on handhold walls the freeform gets discarded here - that is what we waive).
// NO vtable swapping: the move must be the one the game created itself, with
// its own coherent trajectory data.
bool FancyVTableSwapPlugin::OnCreationDiscardOverride(AvailableParkourAction& action)
{
    if (!IsActive()) return false;
    if (GetTypeIntSafe(&action) != (int)EnumParkourAction::wallrunUpFromGroundFailed_mb) return false;
    {
        std::lock_guard lk(m_Mutex);
        ++m_KeptAlive;
    }
    return true;
}

// While armed, discard every naturally-created 53 (wallrunUpFromGround) and 54
// (wallrunUpToHang) upon creation via the vanilla destroy path, so they cannot
// win selection or split the freeform wallrun's decision. Natural 52s kept
// alive above survive and the game picks among them itself.
bool FancyVTableSwapPlugin::OnForceCreationDiscard(AvailableParkourAction& action)
{
    if (!IsActive()) return false;
    const int type = GetTypeIntSafe(&action);
    const bool discard = type == (int)EnumParkourAction::wallrunUpFromGround_mb
                      || type == (int)EnumParkourAction::wallrunUpToHang;
    if (discard)
    {
        std::lock_guard lk(m_Mutex);
        ++m_Discarded;
    }
    return discard;
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
    if (ImGui::Checkbox("Enable Wallrun Discarder (passive: natural freeform 52 only, hotkey ACTIVE)", &enabled))
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
    {
        std::lock_guard lk(m_Mutex);
        ImGui::Text("Kept alive: %d natural freeform 52s this session", m_KeptAlive);
        ImGui::Text("Discarded: %d wallrun 53/54 candidates this session", m_Discarded);
    }
    ImGui::TextWrapped("While armed: wallrun (53) and wallrun-to-overhang (54) candidates are destroyed at creation via the vanilla reject path; natural freeform 52s survive and the game picks them itself. The sort&&select site is never patched, so FreeJump keeps its own normal path. On non-standable objects nothing happens - use FreeJump's overrider there.");
}
