#pragma once

class GroundRollPlugin
{
public:
    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool  m_Enabled      = false;
    int   m_ToggleKey    = VK_F8;
    int   m_ActionKey    = 0x56; // V
    bool  m_PrevKeyState = false;
    int   m_DodgeVK      = 0xA2; // VK_LCONTROL
};
