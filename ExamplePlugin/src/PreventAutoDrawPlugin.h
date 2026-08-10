#pragma once

class PreventAutoDrawPlugin
{
public:
    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    void DoForceSheathe();

    bool  m_Enabled        = false;
};
