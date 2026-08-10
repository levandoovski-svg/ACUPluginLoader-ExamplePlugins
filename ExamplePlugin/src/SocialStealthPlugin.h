#pragma once

class SocialStealthPlugin
{
public:
    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool  m_Enabled = false;
    bool  m_WasBlending = false;
};
