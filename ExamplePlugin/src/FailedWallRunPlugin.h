#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"

class AvailableParkourAction;
class AtomGraph;

struct GPH_FailedWallrunCreator : AutoAssemblerCodeHolder_Base
{
    GPH_FailedWallrunCreator();
};

class FailedWallRunPlugin
{
public:
    FailedWallRunPlugin();
    ~FailedWallRunPlugin();

    void OnUpdate();
    void OnImGuiRender();
    void LoadSettings();
    void SaveSettings();

private:
    static bool IsRisingEdgePressed(int vkCode);

    bool IsFeatureActive() const;
    bool IsInParkourState() const;

    static constexpr uint32 kParkourRTCPIdx = 372;
    static constexpr uint32 kGeneralStateRTCPIdx = 237;

    AutoAssembleWrapper<GPH_FailedWallrunCreator> m_FailedWallrunCreator;
    bool m_CreatorHookActive = false;

    bool m_PluginEnabled = true;
    int m_ToggleKey = 0x76;
    bool m_WaitingForKey = false;
    bool m_HoldMode = true;
    bool m_ToggleState = false;

    static uint64 s_Type52FancyVTable;

    float m_LiftAmount = 2.0f;
    float m_LiftSpeed = 6.0f;

    bool m_EnforceEnabled = true;
    bool m_VtableSwapEnabled = false;

    bool m_CancelEnabled = false;

    // Debug stats
    int m_LastAction52Count = 0;
    int m_LastSabotagedCount = 0;
};
