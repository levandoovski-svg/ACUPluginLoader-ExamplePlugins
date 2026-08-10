#pragma once

#include <cstdint>
#include "ACU/CameraSelectorBlenderNode.h"

// BombSprintAimPlugin
// Route 2 "pseudo-aim": lets the player aim and throw bombs at full sprint speed
// WITHOUT ever leaving the run state (OnGroundHighProfile).
//
// v1 has NO code patches — everything is per-frame OnUpdate logic,
// fully SEH-guarded. All behaviors are independently toggleable from the
// ImGui panel so each layer can be tested/disabled on its own:
//
//   [1] Force BombAim camera  — writes the cached BombAimRegular camera
//       selector block (handle 0x12F9251F30) into the camera component's
//       currentCameraSelectorBlenderNode every frame while pseudo-aiming.
//       The block is captured at runtime the first time the game itself uses
//       that camera mode (e.g. a normal bomb aim), and a +1 strong ref is
//       taken so it can't be freed out from under us.
//   [2] Drive aiming process   — keeps the BallisticProjectileAimingProcess
//       (aiming<EquipType> inside HumanStatesHolder) forward vector aligned
//       with the camera look direction, in the ground plane.
//   [3] Throw on release       — dispatches OnThrowBomb_P (0x141988870) when
//       the aim button is released after being held >= threshold while
//       pseudo-aiming (sprint + aim held).

class BombSprintAimPlugin
{
public:
    BombSprintAimPlugin() = default;
    ~BombSprintAimPlugin() = default;

    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();

private:
    void UpdatePseudoAim();
    void TryForceBombAimCamera();
    void DriveAimingProcess();

    // input / lifecycle
    bool     m_WasAimHeld = false;
    uint64_t m_AimHoldStartTick = 0;

    // settings
    bool m_Enabled          = true;
    bool m_ForceCamera      = true;
    bool m_DriveAimingProcess = true;
    bool m_ThrowOnRelease   = true;
    int  m_ThrowAfterHoldMs = 300;

    // debug / readout
    bool     m_PseudoAiming = false;
    bool     m_CameraIsBombAim = false;
    bool     m_HaveCachedBombAimBlock = false;
    SharedPtrNew<CameraSelectorBlenderNode> m_CachedBombAimBlock;
    int      m_FramesPseudoAiming = 0;
    int      m_ThrowsDispatched = 0;
    int      m_CameraForcesApplied = 0;
};