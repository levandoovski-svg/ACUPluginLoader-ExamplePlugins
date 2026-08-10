#pragma once

#include <string>
#include <cstdint>

class NightCrowdResearchPlugin
{
public:
    void Initialize();
    void OnUpdate();
    void OnImGuiRender();

private:
    std::wstring GetIniPath() const;
    void LoadConfig();
    void SaveConfig();

    bool m_Enabled = false;
    int32_t m_HidePercent = 80;
    int32_t m_TotalHidden = 0;
    int32_t m_TotalTracked = 0;
    bool m_ConfigLoaded = false;
};
