#pragma once

#include "pch.h"
#include "vmath/vmath.h"
#include "ACU/basic_types.h" // uint64 etc. (not provided by pch/vmath)

class ACUPlayerCameraComponent;
struct PhotoModeCameraHook;

// ============================================================
// PhotoModePlugin
//
// Two camera takeover modes for Assassin's Creed Unity:
//   Mode::Free      - "Photo Mode": fly the camera anywhere (WASD/QE move,
//                     mouse orbit, wheel FOV), freeze the world, optional
//                     dotted-out player. Perfect screenshots of Arno.
//   Mode::Cinematic - "Cinematic Follow": camera orbits the player at a fixed
//                     distance/angle so it sticks to him during gameplay.
//
// SAFETY MODEL (learned from BombSprintAimPlugin v1 which crashed by forcing
// the camera-selector field): we NEVER touch the camera selector / mode graph.
// We only override the final camera pose + FOV inside the game's own per-frame
// camera update, at the "setting FOV for frame" instruction (0x141F3FE3B),
// where the ACUPlayerCameraComponent is in r14. Same mechanism as ACUFixes
// FreezeFOV and FreeCameraRotationPlugin.
// ============================================================
class PhotoModePlugin
{
public:
    enum class Mode { None, Free, Cinematic };

    PhotoModePlugin();

    void LoadSettings();
    void SaveSettings();
    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();

    // Called from the camera hook every frame (r14 = ACUPlayerCameraComponent).
    bool ShouldOverrideFrame() const { return m_Enabled && m_Mode != Mode::None; }
    Mode GetMode() const { return m_Mode; }
    void ApplyFreeCamera(ACUPlayerCameraComponent* cam);
    void ApplyCinematicCamera(ACUPlayerCameraComponent* cam);

    // Hook-verification stats (shown in the ImGui panel so you can confirm
    // 0x141F3FE3B is live on your build).
    uint64 m_HookHitCount = 0;
    bool m_HookAlive = false;

private:
    friend struct PhotoModeCameraHook;

    static std::string GetIniPath();
    bool IsRisingEdgePressed(int vkCode);
    void EnterFreeMode();
    void EnterCinematicMode();
    void ExitMode();
    void ResetCamera();
    void SnapCameraFromGame();
    void SetWorldTimescale(float newTimescale);
    void UpdateFreeInput(float dt);
    void UpdateCinematicInput(float dt);
    void ApplyHidePlayer();
    void RestorePlayerVisibility();

    bool m_Enabled = true;
    Mode m_Mode = Mode::None;

    // Hotkeys (rebindable; F9/F10/F11 defaults).
    int m_FreeCamKey = VK_F9;
    int m_CinematicKey = VK_F10;
    int m_ResetKey = VK_F11;
    bool m_WaitingForKey = false;
    int m_RebindTarget = 0; // 1=FreeCamKey, 2=CinematicKey, 3=ResetKey
    bool m_PrevFreeDown = false;
    bool m_PrevCinematicDown = false;
    bool m_PrevResetDown = false;

    // Plugin-owned camera state.
    Vector3f m_FreeCamPos;   // Free mode: absolute camera position.
    float m_Yaw = 0.0f;      // Horizontal orbit angle (both modes).
    float m_Pitch = 0.45f;   // Vertical orbit angle (both modes).
    float m_Distance = 4.0f; // Cinematic mode: distance from the player.
    float m_Fov = 1.0f;      // fov_mb_pi_4 multiplier (0.2 .. 2.0).

    // Snapshot of the game camera taken when entering a mode.
    // "Reset Camera" restores it.
    bool m_HasSnapshot = false;
    Vector3f m_SnapshotPos;
    float m_SnapshotYaw = 0.0f;
    float m_SnapshotPitch = 0.45f;
    float m_SnapshotDistance = 4.0f;
    float m_SnapshotFov = 1.0f;

    // Options.
    bool m_FreezeWorld = true;          // Free mode: timescale 0.
    bool m_HidePlayer = false;          // Free mode: hide Arno (clean shots).
    bool m_SlowMotion = false;          // Cinematic mode: slow timescale.
    float m_SlowMotionTimescale = 0.30f;
    float m_MoveSpeed = 4.0f;           // Free mode: world units / second.
    float m_MouseSensitivity = 0.003f;  // Same default as FreeCameraRotation.
    bool m_InvertX = false;
    bool m_InvertY = false;
    bool m_DisableSmoothing = true;

    // Restore state.
    bool m_TimescaleApplied = false;
    bool m_PlayerHidden = false;
    uint64 m_SavedPlayerFlags88 = 0;

    uint64 m_LastTick = 0;
};

extern PhotoModePlugin* g_pPhotoMode;