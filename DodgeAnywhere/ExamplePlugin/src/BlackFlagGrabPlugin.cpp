#include "pch.h"

#include "BlackFlagGrabPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include <fstream>
#include <Windows.h>
#include <shlobj.h>

static std::string GetBFGSaveFolder()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        std::string path(documents);
        path += "\\Assassin's Creed Unity\\BlackFlagGrabPlugin.ini";
        return path;
    }
    return "BlackFlagGrabPlugin.ini";
}

void BlackFlagGrabPlugin::LoadSettings()
{
    std::ifstream file(GetBFGSaveFolder());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("PluginEnabled=", 0) == 0)
            try { m_PluginEnabled = std::stoi(line.substr(14)) != 0; } catch (...) {}
        else if (line.rfind("ToggleKey=", 0) == 0)
            try { m_ToggleKey = std::stoi(line.substr(10), nullptr, 16); } catch (...) {}
        else if (line.rfind("HoldMode=", 0) == 0)
            try { m_HoldMode = std::stoi(line.substr(9)) != 0; } catch (...) {}
        else if (line.rfind("ToggleState=", 0) == 0)
            try { m_ToggleState = std::stoi(line.substr(12)) != 0; } catch (...) {}
    }
}

void BlackFlagGrabPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetBFGSaveFolder());
        if (file)
        {
            file << "PluginEnabled=" << (m_PluginEnabled ? 1 : 0) << "\n";
            file << "ToggleKey="     << std::hex << m_ToggleKey << std::dec << "\n";
            file << "HoldMode="      << (m_HoldMode ? 1 : 0) << "\n";
            file << "ToggleState="   << (m_ToggleState ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

void BlackFlagGrabPlugin::OnBeforeActivate()
{
    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction*
    {
        return static_cast<BlackFlagGrabPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.ChooseAfterSorting_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame) -> AvailableParkourAction*
    {
        return static_cast<BlackFlagGrabPlugin*>(userData)->ChooseAfterSorting(actions, selectedByGame);
    };
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 5.0f;
    m_Callbacks.m_Name = "BlackFlagGrabPlugin";

    LoadSettings();
}

void BlackFlagGrabPlugin::OnBeforeDeactivate()
{
    if (m_HookActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActivatorCreation.reset();
        m_HookActive = false;
    }
    SaveSettings();
}

void BlackFlagGrabPlugin::OnUpdate()
{
    // Handle key rebinding
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (IsRisingEdgePressed(vk))
            {
                m_ToggleKey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
        return; // don't process key states while waiting for a key
    }

    // Update toggle state if in toggle mode and key pressed
    if (m_PluginEnabled && !m_HoldMode)
    {
        if (IsRisingEdgePressed(m_ToggleKey))
        {
            m_ToggleState = !m_ToggleState;
            SaveSettings();
        }
    }

    // Determine if the feature should be active (hook should run)
    bool shouldBeActive = false;
    if (m_PluginEnabled)
    {
        if (m_HoldMode)
            shouldBeActive = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
        else
            shouldBeActive = m_ToggleState;
    }

    // Activate/deactivate hook
    if (shouldBeActive && !m_HookActive)
    {
        auto gph = GenericHooksInParkourFiltering::GetSingleton();
        gph->Subscribe(m_Callbacks);
        m_HookActivator = gph->RequestGPHSortAndSelect();
        m_HookActivatorCreation = gph->RequestGPHCreation();
        m_HookActive = true;
    }
    else if (!shouldBeActive && m_HookActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActivatorCreation.reset();
        m_HookActive = false;
    }
}

void BlackFlagGrabPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
    const ImVec4 warnColor(1.00f, 0.80f, 0.00f, 1.00f);
    const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);

    ImGui::Text("Black Flag Move");
    ImGui::Separator();

    ImGui::TextColored(warnColor, "WARNING: May cause clipping or falling through world. Save before using.");
    ImGui::Spacing();

    ImGui::TextWrapped("Suppresses dominant climb/wall actions, allowing unusual geometry-dependent grab animations.\nMost visible when Arno is near protruding surfaces (beams, ropes).");
    ImGui::Spacing();

    // Master enable checkbox
    bool enabled = m_PluginEnabled;
    if (ImGui::Checkbox("Enable Black Flag Move", &enabled))
    {
        m_PluginEnabled = enabled;
        SaveSettings();
    }

    if (m_PluginEnabled)
    {
        ImGui::Indent();

        // Status display
        bool isActive = false;
        if (m_HoldMode)
            isActive = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
        else
            isActive = m_ToggleState;
        ImGui::Text("Status:"); ImGui::SameLine();
        ImGui::TextColored(isActive ? activeColor : inactiveColor, isActive ? "ACTIVE" : "INACTIVE");

        // Mode selection
        bool holdMode = m_HoldMode;
        if (ImGui::RadioButton("Hold (key held)", holdMode))
        {
            m_HoldMode = true;
            SaveSettings();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Toggle (press once to toggle)", !holdMode))
        {
            m_HoldMode = false;
            SaveSettings();
        }

        // Key rebind
        ImGui::Text("Trigger key: 0x%02X", m_ToggleKey);
        if (m_WaitingForKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Press any key..."); }
        if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind##bfg"))
            m_WaitingForKey = !m_WaitingForKey;

        ImGui::Unindent();
    }
}

AvailableParkourAction* BlackFlagGrabPlugin::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    // Check if plugin is enabled and active (hook is only called when shouldBeActive is true)
    // But we also need to check the current active state because the hook could be activated but the key might have been released between frames? No, hook activation tracks shouldBeActive, so if we're here, it's active.
    // However, to be safe, we re-check the active state using the current key/toggle state.
    if (!m_PluginEnabled) return nullptr;

    bool isActive = m_HoldMode ? (GetAsyncKeyState(m_ToggleKey) & 0x8000) : m_ToggleState;
    if (!isActive) return nullptr;

    // Original core logic (unchanged)
    for (int i = 0; i < actions.size; ++i)
    {
        AvailableParkourAction* action = actions[i];
        if (!action) continue;
        int t = (int)action->GetEnumParkourAction();

        if (t == 52) // wallrunUpFromGroundFailed_mb — boost so it survives cull
        {
            action->fitness = 9999.0f;
        }
        else if (t == 53 || t == 54 || t == 65 || t == 73 || t == 32 || t == 44 || t == 77 || t == 70 || t == 35)
        {
            action->fitness = 0.0f;
        }
    }
    return nullptr;
}

AvailableParkourAction* BlackFlagGrabPlugin::ChooseAfterSorting(SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame)
{
    if (!m_PluginEnabled) return selectedByGame;

    bool isActive = m_HoldMode ? (GetAsyncKeyState(m_ToggleKey) & 0x8000) : m_ToggleState;
    if (!isActive) return selectedByGame;

    // If action 52 survived the filters, force it as the final selection.
    for (int i = 0; i < actions.size; ++i)
    {
        AvailableParkourAction* action = actions[i];
        if (!action) continue;
        if ((int)action->GetEnumParkourAction() == 52)
            return action;
    }
    return selectedByGame;
}

bool BlackFlagGrabPlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}