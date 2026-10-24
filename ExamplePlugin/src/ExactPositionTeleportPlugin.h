#pragma once

#include <array>
#include <cstdint>
#include <string>

// Exact-position teleport / force-freefall test plugin.
//
// A rising press of the configured force key:
//   1. (optional) runs a raycast ledge/wall gate around the player, so the
//      force-freefall cannot be triggered in mid-air with no ledge nearby,
//   2. snapshots the player's current transform and position,
//   3. writes that unchanged transform/position back through the game's
//      BaseEntity::SetTransform path,
//   4. dispatches the same HSTE ResetBehavior used by ACUFixes' debug
//      TeleportPlayer helper so the game re-evaluates the current context,
//   5. requests EnumParkourAction::breakfall through the game's normal
//      parkour-action request pipeline. While HoldBreakfallWhileHeld is on,
//      that breakfall request is re-issued every frame for as long as the
//      force key remains held, and stops on release.
//
// The same-position teleport/reset behavior was confirmed by the tester to put
// the player into freefall when executed mid-air.
class ExactPositionTeleportPlugin {
public:
    void Init();
    void OnUpdate();
    void OnImGuiRender();

private:
    void BeginForceFreefall();
    bool TeleportToExactCurrentPosition();
    bool ProbeLedgeGate();
    void ServiceHeldBreakfall();
    void ProcessPendingBreakfall();
    void ReleaseForce();

    void LoadSettings();
    void SaveSettings();
    bool EnsureIniFile();
    std::string ResolvePrimaryIniPath() const;

    void SetStatus(const std::string& status);
    void Log(const std::string& line) const;
    std::string GetLogPath() const;

    // key / general
    int  m_ForceKey = 'T';                 // Windows virtual-key code
    bool m_WaitingForKey = false;
    std::array<bool, 256> m_CaptureKeyWasDown {};

    // breakfall
    bool m_BreakfallAfterTeleport = true;
    bool m_HoldBreakfallWhileHeld = true;
    int  m_BreakfallActionId = 18;         // EnumParkourAction::breakfall
    int  m_BreakfallDelayMs = 100;         // delay before the first request
    int  m_BreakfallHoldIntervalMs = 0;    // 0 = re-issue every frame while held

    // ledge gate (raycast)
    bool  m_RequireLedge = true;
    int   m_ProbeDirections = 12;
    float m_MaxHorizontalReach = 1.5f;
    float m_WallProbeHeight = 0.9f;
    float m_WallNormalMaxAbsZ = 0.35f;
    float m_LedgeInset = 0.35f;
    float m_MinLedgeRise = -1.0f;
    float m_MaxLedgeRise = 2.5f;
    float m_TopNormalMinZ = 0.6f;

    // runtime state
    bool m_BuildCompatible = false;
    bool m_RaycastCompatible = false;
    bool m_BreakfallRequestCompatible = false;
    bool m_PrevKeyDown = false;
    bool m_ForceActive = false;
    bool m_PendingBreakfall = false;
    bool m_LastTransformApplied = false;
    bool m_LastResetDispatched = false;
    bool m_LastBreakfallRequested = false;
    int  m_HoldRequestCount = 0;
    uint64_t m_BreakfallRequestAt = 0;
    uint64_t m_NextHoldRequestAt = 0;

    bool  m_LastProbeFound = false;
    int   m_LastProbeRays = 0;
    int   m_LastProbeWalls = 0;
    float m_LastProbeDistance = 0.0f;
    float m_LastProbeRise = 0.0f;

    float m_LastX = 0.0f;
    float m_LastY = 0.0f;
    float m_LastZ = 0.0f;
    std::string m_IniPath;
    std::string m_LastStatus = "Not initialized";
};
