#pragma once

#include <string>

// A deliberately small test harness for the "same-position teleport cancels
// the active parkour/freefall transition" theory.
//
// On a rising T press it:
//   1. snapshots the player's current transform and position,
//   2. writes that unchanged transform/position back through the game's
//      BaseEntity::SetTransform path, and
//   3. dispatches the same HSTE ResetBehavior used by ACUFixes' debug
//      TeleportPlayer helper so the game re-evaluates the current context.
//
// Step 3 is intentionally the library-proven part of a teleport. It may also
// be the part that cancels the parkour action, so this plugin is a test rig,
// not proof that an identical-coordinate write cancels a move by itself.
class ExactPositionTeleportPlugin {
public:
    void Init();
    void OnUpdate();
    void OnImGuiRender();

private:
    void TeleportToExactCurrentPosition();
    void SetStatus(const std::string& status);
    void Log(const std::string& line) const;
    std::string GetLogPath() const;

    bool m_BuildCompatible = false;
    bool m_PrevKeyDown = false;
    bool m_LastTransformApplied = false;
    bool m_LastResetDispatched = false;

    float m_LastX = 0.0f;
    float m_LastY = 0.0f;
    float m_LastZ = 0.0f;
    std::string m_LastStatus = "Not initialized";
};
