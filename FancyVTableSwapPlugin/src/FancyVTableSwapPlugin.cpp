#include "pch.h"
#include "FancyVTableSwapPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/GetTypeIntSafe.h"
#include "imgui/imgui.h"
#include <fstream>
#include <shlobj.h>
#include <cstdio>
#include <set>

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
        return std::string(docs) + "\\Assassin's Creed Unity\\FreeFormWallrun.ini";
    return "FreeFormWallrun.ini";
}
// Pre-rename INI, read once on load so existing hotkey settings survive.
static std::string GetLegacyIniPath()
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
    m_Callbacks.m_Name = "FreeFormWallrun_DiscardWallruns";
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
    if (!file) { file.clear(); file.open(GetLegacyIniPath()); }
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
    return GetTypeIntSafe(&action) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
}

// While armed, discard every naturally-created 53 (wallrunUpFromGround) and 54
// (wallrunUpToHang) upon creation via the vanilla destroy path, so they cannot
// win selection or split the freeform wallrun's decision. Natural 52s kept
// alive above survive and the game picks among them itself.
bool FancyVTableSwapPlugin::OnForceCreationDiscard(AvailableParkourAction& action)
{
    if (!IsActive()) return false;
    const int type = GetTypeIntSafe(&action);
    return type == (int)EnumParkourAction::wallrunUpFromGround_mb
        || type == (int)EnumParkourAction::wallrunUpToHang;
}

void FancyVTableSwapPlugin::OnUpdate()
{
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            // Held RIGHT NOW and NOT already down when capture was armed =
            // a genuinely fresh press. The old `& 1` transition bit carries
            // stray edge states from previous frames, which made rebinding
            // cycle through random keys.
            if ((GetAsyncKeyState(vk) & 0x8000) && !m_KeysDownAtCapture.count(vk))
            {
                m_Hotkey = vk;
                m_WaitingForKey = false;
                m_KeysDownAtCapture.clear();
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
    if (ImGui::Checkbox("Enable FreeFormWallrun (passive: natural freeform 52 only, hotkey ACTIVE)", &enabled))
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
    {
        if (!m_WaitingForKey)
        {
            // Snapshot keys already down (including the mouse button that
            // just clicked this Rebind button): rebinding only accepts
            // presses that start AFTER this moment.
            m_KeysDownAtCapture.clear();
            for (int vk = 1; vk <= 0xFE; ++vk)
                if (GetAsyncKeyState(vk) & 0x8000)
                    m_KeysDownAtCapture.insert(vk);
        }
        m_WaitingForKey = !m_WaitingForKey;
    }
    if (m_WaitingForKey) ImGui::Text("Press any key to bind...");
}
