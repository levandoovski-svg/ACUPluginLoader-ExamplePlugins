#pragma once
#include <cstdint>

class PrecisionParkourPlugin
{
public:
    void LoadSettings();
    void Initialize();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool m_Enabled = false;
    int  m_KeyBind = 46;

    std::string GetIniPath();
    void SaveSettings();
};
