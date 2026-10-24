#pragma once

#include <cstdint>
#include <string>

// Exact-position teleport / force-freefall test plugin.
//
// A rising press of the configured force key:
//   1. snapshots the player's current transform and position,
//   2. writes that unchanged transform/position back through the game's
//      BaseEntity::SetTransform path,
//   3. dispatches the same HSTE ResetBehavior used by ACUFixes' debug
//      TeleportPlayer helper so the game re-evaluates the current context,
//   4. optionally requests EnumParkourAction::breakfall (18) through the
//      game's normal parkour-action request pipeline.
//
// The same-position teleport/reset behavior was confirmed by the tester to put
// the player into freefall when executed mid-air. Breakfall is optional so the
// teleport-only behavior can still be tested independently.
class ExactPositionTeleportPlugin {
public:
    void Init();
    void OnUpdate();
    void OnImGuiRender();

private:
    void TriggerForceFreefall();
    bool TeleportToExactCurrentPosition();
    void ProcessPendingBreakfall();

    void LoadSettings();
    void SaveSettings();
    bool EnsureIniFile();
    std::string ResolvePrimaryIniPath() const;

    void SetStatus(const std::string& status);
    void Log(const std::string& line) const;
    std::string GetLogPath() const;

    int  m_ForceKey = 'T';                 // Windows virtual-key code
    bool m_BreakfallAfterTeleport = true;
    int  m_BreakfallDelayMs = 100;         // delay before requesting action 18
    bool m_WaitingForKey = false;

    bool m_BuildCompatible = false;
    bool m_BreakfallRequestCompatible = false;
    bool m_PrevKeyDown = false;
    bool m_LastTransformApplied = false;
    bool m_LastResetDispatched = false;
    bool m_LastBreakfallRequested = false;
    bool m_PendingBreakfall = false;
    uint64_t m_BreakfallRequestAt = 0;

    float m_LastX = 0.0f;
    float m_LastY = 0.0f;
    float m_LastZ = 0.0f;
    std::string m_IniPath;
    std::string m_LastStatus = "Not initialized";
};
