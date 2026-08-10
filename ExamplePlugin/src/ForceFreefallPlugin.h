#pragma once
#include <cstdint>

class ForceFreefallPlugin
{
public:
    void Init();
    void LoadSettings();
    void SaveSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    int  m_ActivateKey = VK_F6;
    bool m_WaitingForKey = false;
};
