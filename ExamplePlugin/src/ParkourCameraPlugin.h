#pragma once

class ParkourCameraPlugin
{
public:
    void LoadSettings();
    void SaveSettings();
    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();
    void OnOverlayRender();

private:
    static bool IsRisingEdgePressed(int vkCode);

    int m_ToggleKey = VK_XBUTTON1;
    bool m_WaitingForKey = false;

    int m_CenterScreenKey = VK_XBUTTON2;
    bool m_WaitingForCenterKey = false;
    bool m_CenterScreenMode = false;

    bool m_ShowCrosshair = true;
};
