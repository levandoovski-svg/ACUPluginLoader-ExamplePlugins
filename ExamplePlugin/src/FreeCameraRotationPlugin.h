#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"

class FreeCameraRotationPlugin
{
public:
    void OnUpdate();
    void OnImGuiRender();
    void Initialize();

    bool m_Enabled = true;
    bool m_InvertY = false;
    bool m_InvertX = false;
    bool m_SlowMotion = false;
    float m_SlowMotionTimescale = 0.2f;
    bool m_WasInAssassination = false;
    bool m_TimescaleApplied = false;

    bool IsInAssassinationState();
};

extern FreeCameraRotationPlugin g_FreeCameraRotation;
