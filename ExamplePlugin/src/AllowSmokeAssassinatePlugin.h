#pragma once

class AllowSmokeAssassinatePlugin
{
public:
    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool   m_Enabled = false;
};
