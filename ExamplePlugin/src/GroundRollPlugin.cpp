#include "pch.h"
#include "GroundRollPlugin.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>

static void SimulateKeyPress(uint8_t vk)
{
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\GroundRoll.ini";
    return "GroundRoll.ini";
}

void GroundRollPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("ToggleKey=", 0) == 0)
            try { m_ToggleKey = std::stoi(line.substr(10), nullptr, 16); } catch (...) {}
        if (line.rfind("ActionKey=", 0) == 0)
            try { m_ActionKey = std::stoi(line.substr(10), nullptr, 16); } catch (...) {}
    }
}

void GroundRollPlugin::OnUpdate()
{
    // Toggle enable on toggle key press
    bool keyDown = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    if (keyDown && !m_PrevKeyState)
    {
        m_Enabled = !m_Enabled;
        try {
            std::ofstream f(GetIniPath());
            if (f) f << "ToggleKey=" << std::hex << m_ToggleKey << "\n"
                     << "ActionKey=" << std::hex << m_ActionKey << "\n";
        } catch (...) {}
    }
    m_PrevKeyState = keyDown;

    if (!m_Enabled) return;

    // On action key press, simulate dodge (ground roll)
    if (GetAsyncKeyState(m_ActionKey) & 0x8000)
        SimulateKeyPress(m_DodgeVK);
}

void GroundRollPlugin::OnImGuiRender()
{
    ImGui::Checkbox("Ground Roll (press V)", &m_Enabled);
}
