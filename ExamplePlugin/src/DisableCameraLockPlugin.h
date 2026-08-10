#pragma once

#include <cstdint>

class DisableCameraLockPlugin
{
public:
    void LoadSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool  m_Enabled = false;

    // Camera orbit override
    float m_Sensitivity = 0.03f;
    float m_CameraAngleH = 0.0f;
    float m_CameraAngleV = 0.3f;
    float m_CameraDistance = 5.0f;
    bool  m_InvertY = true;
    bool  m_InvertX = false;
    int   m_PrevMouseX = 0;
    int   m_PrevMouseY = 0;

    // Auto-calibration tracking
    float m_LastAngleZ = 0.0f;
    float m_LastAngleV = 0.0f;

    // Slow motion
    bool  m_SlowMotion = false;
    float m_SlowMotionTimescale = 0.2f;
    bool  m_TimescaleApplied = false;

    bool IsInAssassinationState();
};
