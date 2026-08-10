#pragma once
#include <cstdint>
#include <vector>

class AutoDesyncOnAssassinatePlugin
{
public:
    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool m_Enabled = false;

    // Debug readout state, refreshed every OnUpdate call.
    bool                     m_Debug_HolderValid = false;
    int                      m_Debug_ReceiverCount = 0;
    bool                     m_Debug_MatchedThisFrame = false;
    std::vector<uint64_t>    m_Debug_LastSeenAddrs; // up to a handful of Enter addrs seen this frame

    // GetPlayerHealth() chain, broken out step by step.
    bool m_Debug_CameraComponentValid = false;
    bool m_Debug_PlayerEntityValid    = false;
    bool m_Debug_BhvAssassinValid     = false;
    bool m_Debug_HealthValid          = false;

    uint64_t m_Debug_ModuleBase = 0;
};

