#pragma once
#include <cstdint>
#include <string>

class PlayAnimationPlugin
{
public:
    void LoadSettings();
    void SaveSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool    m_Enabled      = false;
    int     m_ToggleKey    = VK_F8;
    uint64  m_AnimHandle   = 27178795240;
    bool    m_PrevKeyState = false;
};
