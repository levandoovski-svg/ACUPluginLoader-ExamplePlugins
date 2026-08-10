#pragma once

#include "pch.h"

class FirstPersonAimPlugin
{
public:
    bool m_Enabled = true;
    bool m_HidePlayer = true;

    float m_FovZoom = 0.3f;

    bool m_WasAiming = false;
    uint64 m_SavedPlayerFlags88 = 0;

    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

    bool IsAimingGun();
};
