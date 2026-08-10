#pragma once

#include <string>

class CameraFacingEjectPlugin
{
public:
    void LoadSettings();
    void SaveSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    void AlignPlayerFacingToCamera();
    std::string GetIniPath();

    int m_ArmKey = VK_XBUTTON1;
    bool m_WaitingForKey = false;
    bool m_PluginEnabled = true;

    bool m_PrevSpaceDown = false;

    float m_DelaySeconds = 1.2f;
    bool m_PendingRotation = false;
    uint64 m_PendingStartTick = 0;
};
