#pragma once

#include "pch.h"
#include "vmath/vmath.h"
#include "ACU/basic_types.h" // uint64 etc. (not provided by pch/vmath)

class ACUPlayerCameraComponent;
struct PhotoModeCameraHook;

// ============================================================
// PhotoModePlugin
//
// Camera takeover mode for Assassin's Creed Unity:
//   Mode::Free - "Photo Mode": fly the camera anywhere (arrow keys + Q/E move,
//                mouse orbit, wheel FOV), freeze the world, optional hidden
//                player. Follow Player anchors the camera to Arno so he stays
//                controllable during gameplay; Freeze Camera locks the pose in
//                place; Tilt Mode enables the rolled look on mouse yaw.
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
    enum class Mode { None, Free };

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

    // Hook-verification stats (kept internal).
    // (debug UI removed for ship-ready build)

private:
    friend struct PhotoModeCameraHook;

    static std::string GetIniPath();
    bool IsRisingEdgePressed(int vkCode);
    void EnterFreeMode();
    void ExitMode();
    void ResetCamera();
    void SnapCameraFromGame();
    void SetWorldTimescale(float newTimescale);
    void UpdateFreeInput(float dt);
    int SaveCurrentCameraSlot();
    void ApplyCameraSlot(int index);
    void CycleCameraSlots(int dir);
    void ApplyHidePlayer();
    void RestorePlayerVisibility();

    bool m_Enabled = true;
    Mode m_Mode = Mode::None;

    // Hotkeys (rebindable; F9/F11 defaults).
    int m_FreeCamKey = VK_F9;
    int m_ResetKey = VK_F11;
    bool m_WaitingForKey = false;
    int m_RebindTarget = 0; // 1=FreeCamKey, 3=ResetKey
    bool m_PrevFreeDown = false;
    bool m_PrevResetDown = false;

    // Plugin-owned camera state.
    Vector3f m_FreeCamPos;   // Free mode: absolute camera position.
    Vector3f m_FollowOffset; // Free mode + Follow Player: camera offset from Arno.
    float m_Yaw = 0.0f;      // Horizontal orbit angle.
    float m_Pitch = 0.45f;   // Vertical orbit angle.
    float m_Fov = 1.0f;      // fov_mb_pi_4 multiplier (0.2 .. 2.0).

    // Saved camera poses (panel + ',' / '.' switching).
    struct CameraSlot
    {
        bool used = false;
        Vector3f pos;
        float yaw = 0.0f;
        float pitch = 0.45f;
        float fov = 1.0f;
        float tilt = 0.0f;
        bool relativeToPlayer = false;  // If true, pos is offset from player; if false, pos is world position
    };
    CameraSlot m_Slots[9];
    int m_ActiveSlot = -1;
    bool m_PrevCommaDown = false;
    bool m_PrevPeriodDown = false;

    // Slot hotkeys (1-9 for slots 1-9; default to numeric keys).
    int m_SlotKeys[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    bool m_PrevSlotKeyDown[9] = { false, false, false, false, false, false, false, false, false };
    
    // Numpad slot hotkeys (Numpad 1-9; default to numpad keys).
    int m_NumpadSlotKeys[9] = { VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9 };
    bool m_PrevNumpadSlotKeyDown[9] = { false, false, false, false, false, false, false, false, false };

    // Snapshot of the game camera taken when entering a mode.
    // "Reset Camera" restores it.
    bool m_HasSnapshot = false;
    Vector3f m_SnapshotPos;
    float m_SnapshotYaw = 0.0f;
    float m_SnapshotPitch = 0.45f;
    float m_SnapshotFov = 1.0f;

    // Options.
    bool m_FreezeWorld = true;          // Free mode: timescale 0.
    bool m_HidePlayer = false;          // Free mode: hide Arno (clean shots).
    bool m_FollowPlayer = false;        // Free mode: camera tracks Arno, world stays live.
    bool m_FollowAllowMouse = false;    // Follow Player: legacy toggle — mouse orbit/FOV allowed; arrows & Q/E stay locked.
    bool m_FollowIgnoreAllInput = true; // Follow Player: ignore ALL camera movement input by default (preserves previous behavior)
    bool m_FollowIgnoreExceptMouse = false; // Follow Player: ignore camera movement input except allow mouse control
    bool m_FreezeCamera = false;        // Free mode: lock camera pose, play without moving it.
    bool m_TiltMode = false;            // Free mode: rolled camera on mouse yaw (quat write).
    float m_TiltAngle = 0.0f;           // Degrees; static roll in normal freecam (carry-over from Tilt Mode).
    bool m_MouseTilt = true;            // Mouse X adjusts Tilt Angle instead of yaw (dial roll by feel). ON by default.
    float m_MouseTiltSensitivity = 3.0f; // Multiplier for how strongly mouse X affects tilt when MouseTilt is on.
    // Disable individual camera axes (prevents user input from modifying these axes)
    bool m_DisableYaw = true;           // Yaw is disabled by default; enabling Tilt Mode will re-enable yaw.
    bool m_DisablePitch = false;
    bool m_DisableRoll = false;
    // Block camera POSITION movement but allow angle movement (while following, centers on player)
    // m_BlockPositionMovement removed; kept behavior simple.
    float m_MoveSpeed = 4.0f;           // Free mode: world units / second.
    float m_MouseSensitivity = 0.003f;  // Same default as FreeCameraRotation.
    bool m_InvertX = false;
    bool m_InvertY = false;
    bool m_DisableSmoothing = true;
    bool m_SaveSlotsRelativeToPlayer = false; // When true, save slots as offset from player pos instead of world pos

    // Restore state.
    bool m_TimescaleApplied = false;
    bool m_PlayerHidden = false;
    uint64 m_SavedPlayerFlags88 = 0;

    uint64 m_LastTick = 0;

    // Recording/replay removed for ship-ready build.
};

extern PhotoModePlugin* g_pPhotoMode;