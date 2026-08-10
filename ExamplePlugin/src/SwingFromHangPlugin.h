#pragma once
#include <cstdint>

class SwingFromHangPlugin
{
public:
    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool m_Enabled = true;
    bool m_HoldMode = true;
    bool m_ToggleState = false;
    int  m_ToggleKey = VK_LSHIFT;
    bool m_WaitingForKey = false;

    float m_LiftAmount = 1.5f;
    int   m_SwingType = 43;
    bool  m_DebugMode = false;

    bool m_PrevKeyState = false;

    bool IsFeatureActive() const;
    bool IsRisingEdgePressed(int vkCode);
    bool IsInHangState() const;
    void LoadSettingsFile();
    void SaveSettings();
    std::string GetIniPath();
};
