#pragma once
#include <cstdint>

class ParryAssassinPlugin
{
public:
    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    int     m_ParryCount         = 0;
    float   m_LastParryTime      = 0.0f;
    bool    m_Enabled            = true;
    bool    m_ReadyToAssassinate = false;
    bool    m_PrevParryState     = false;
    bool    m_PrevReady          = false;

    int     m_TriggerKey          = VK_SPACE;
    bool    m_TriggerKeyPrev      = false;
    float   m_OverrideStartTime   = 0.0f;
    bool    m_WaitingForKeybind   = false;

    bool IsParryActive();

    static constexpr uint64_t ADDR_PARRYING_PUNCHENEMY = 0x1419AAA90;
};
