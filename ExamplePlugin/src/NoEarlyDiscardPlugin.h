#pragma once
#include <memory>
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"

struct ParkourCreationHook : AutoAssemblerCodeHolder_Base
{
    ParkourCreationHook();
};

class NoEarlyDiscardPlugin
{
public:
    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();

private:
    AutoAssembleWrapper<ParkourCreationHook> m_CreationHook;
    bool m_HookActive = false;

    bool m_Enabled = false;
    bool m_Protect39 = true;
    float m_HeightMin = -4.0f;
    float m_HeightMax = 2.0f;
    float m_HorizMin = -4.0f;
    float m_HorizMax = 4.0f;
    float m_DistMin = 0.0f;
    float m_DistMax = 8.0f;
    float m_CurveMin = -5.0f;
    float m_CurveMax = 5.0f;
    float m_Fitness = 9999.0f;
};
