#pragma once
#include <cstdint>

class DodgeAnywherePlugin
{
public:
    void OnUpdate();
    void OnImGuiRender();

private:
    bool m_Enabled = true;
    bool m_WaitingForKey = false;
    int  m_Key = VK_SPACE;

    bool m_RestoreNextFrame = false;
    bool m_SavedCounterDodge = false;
    bool m_SavedCanDodgeRange = false;
    bool m_SavedCombatSteps = false;
};