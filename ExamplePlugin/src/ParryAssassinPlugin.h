#pragma once
#include <cstdint>

class ParryAssassinPlugin
{
public:
    void LoadSettings();
    void SaveSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    int     m_ParryCount         = 0;
    float   m_LastParryTime      = 0.0f;
    bool    m_Enabled            = true;
    bool    m_ReadyToAssassinate = false;
    bool    m_PrevParryState     = false;
    bool    m_PrevReady          = false;

    bool    m_CritKillEnabled         = false;
    bool    m_BoostCritDamage         = true;
    bool    m_UnrestrictedCritEnabled = true;
    bool    m_PatchesActive           = false;

    // Visual cue: gold highlight on the currently-attacked enemy while ready.
    bool    m_HighlightCueEnabled     = true;
    bool    m_ForceCriticalState      = true;
    bool    m_StrictTargetCheck       = true;
    float   m_GoldHighlightColor[4]   = { 1.0f, 0.84f, 0.0f, 0.0f };

    bool IsParryActive();
    void UpdateCritPatches();
    void SyncHighlightCueGlobals();

    static constexpr uint64_t ADDR_PARRYING_PUNCHENEMY = 0x1419AAA90;
};
