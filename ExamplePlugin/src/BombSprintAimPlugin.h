#pragma once

#include <cstdint>

// BombSprintAimPlugin - v1.5 (OBSERVE-ONLY diagnostic build)
//
// v1 attempted "pseudo-aim": forcing the game's live camera-selector state
// (BombAim camera block) and ballistic aiming-process members every frame
// while sprint+aim were held. The game crashed on entering aim/bomb-aim while
// running: writing the camera-selector field from outside the game's own
// switch machinery leaves the selector graph inconsistent, and the crash
// lands later inside the game's camera update - outside our SEH guards.
// Lesson: never mutate live camera/aiming state per-frame (direct conflict).
//
// v1.5 wrote NOTHING and only OBSERVED, to locate where the game refuses
// sprint->aim. v2 adds the two sprint-safe "aiming visuals" the user actually
// wants:
//   A) the bomb-aim FOV — a single scalar write to the camera's live FOV
//      fields (fov_mb_pi_4 / fovPrecalc). Nothing structural: no selector
//      graph, no refcounts, no node swap — cannot reproduce the v1 crash.
//   B) our own ballistic arc overlay ("custom raycast") — read-only camera
//      pose + calibrated throw constants, drawn as ImGui background lines.
// Neither enters the AimBomb state, neither writes any game-owned structure.
//
// End-goal direction (v2, after the refusal site is located): the SAFE pattern
// is "decision-level unlock" - patch only the input gate(s) that refuse
// aim-while-sprinting (the same CCodeInTheMiddle recipe ACUFixes uses for
// DisableBombAimInHaystack / MoreResponsiveBombQuickDrop), then let the game
// enter its REAL AimBomb state: real camera, real predictor, real throw on
// release. No per-frame writes at all.

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
    void DriveAimingProcess();
    void ApplyAimFov();
    void DrawArcOverlay();

    // settings
    bool m_Enabled = true;
    // DIAGNOSTIC ONLY: drives the ballistic aiming process forward vector from
    // the camera look. Default OFF. Kept only for A/B isolation - the v1 crash
    // is attributed to the camera forcing, not this, but leave it off.
    bool m_DriveAimingProcess = false;

    // v2 aiming visuals — the sprint-safe way to "line up the throw":
    //   A) scalar FOV write (bomb-aim zoom) while sprint-aiming, and
    //   B) our own read-only ballistic arc overlay (no game state touched).
    bool  m_AimFovEnabled = true;
    float m_AimFovRadians = 0.55f; // game's own bomb-aim FOV (~0.55 rad); slider-calibrated
    bool  m_ArcOverlay    = true;
    bool  m_WorldZUp      = true;  // ACU world orientation; flip if arc looks mirrored
    float m_ThrowSpeed    = 9.0f;  // m/s; calibrate against the real arc in vanilla aim
    float m_Gravity       = 9.81f; // m/s^2; same calibration

    // readout
    bool m_PseudoAiming = false;
    bool m_CameraIsBombAim = false;
    int  m_FramesPseudoAiming = 0;
};