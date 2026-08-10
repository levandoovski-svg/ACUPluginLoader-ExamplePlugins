#pragma once
#include <cstdint>

class AnimationOverridePlugin
{
public:
    void LoadSettings();
    void SaveSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool   m_Enabled         = false;
    int    m_ToggleKey       = VK_F10;
    bool   m_WaitingForKey   = false;
    bool   m_PrevKeyState    = false;
};
