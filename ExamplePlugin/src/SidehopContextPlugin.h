#pragma once
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"

struct SidehopEnablerHooks : AutoAssemblerCodeHolder_Base
{
    SidehopEnablerHooks();
};

class SidehopContextPlugin
{
public:
    SidehopContextPlugin();
    ~SidehopContextPlugin();

    void LoadSettings();
    void SaveSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    AutoAssembleWrapper<SidehopEnablerHooks> m_Hooks;
    bool m_HooksActive = false;

    bool m_PluginEnabled = true;
    int m_Hotkey = 0x76; // F7 (F8 is SidehopNopPlugin's default)
    bool m_WaitingForKey = false;

    // Range extension values
    float m_RangeHeightDiffMin = -3.0f;
    float m_RangeHeightDiffMax = 1.5f;
    float m_RangeExpectedHeightDiff = -2.0f;
    float m_RangeExpectedVertSpeed = 9.0f;
    float m_RangeExpectedHorizSpeed = 4.0f;
    float m_RangeCurveMax = 2.0f;
    float m_RangeCurveMin = -4.0f;
    float m_RangeExpectedCurveMin = -2.0f;
};
