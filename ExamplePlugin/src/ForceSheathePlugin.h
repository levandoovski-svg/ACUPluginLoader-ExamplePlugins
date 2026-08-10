#pragma once
#include <cstdint>

class ForceSheathePlugin
{
public:
    void LoadSettings();
    void SaveSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    // Force sheathe on keypress
    int  m_SheatheKey         = VK_F7;
    bool m_WaitingForSheathe  = false;
    bool m_PrevSheatheKey     = false;

    // Prevent draw toggle
    bool m_PreventDraw        = false;
    int  m_PreventDrawKey     = VK_F8;
    bool m_WaitingForDrawKey  = false;
    bool m_PrevDrawKey        = false;

    static void DoSheathe();
};
