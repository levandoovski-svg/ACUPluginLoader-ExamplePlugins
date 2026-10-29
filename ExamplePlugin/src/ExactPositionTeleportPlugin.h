#pragma once

#include <array>
#include <cstdint>
#include <string>

// Bindable XInput codes use a separate range so existing keyboard/mouse INI
// values remain unchanged.
enum ExactTeleportPadCode
{
    ExactTeleportPad_DPadUp = 0,
    ExactTeleportPad_DPadDown,
    ExactTeleportPad_DPadLeft,
    ExactTeleportPad_DPadRight,
    ExactTeleportPad_Start,
    ExactTeleportPad_Back,
    ExactTeleportPad_L3,
    ExactTeleportPad_R3,
    ExactTeleportPad_LB,
    ExactTeleportPad_RB,
    ExactTeleportPad_A,
    ExactTeleportPad_B,
    ExactTeleportPad_X,
    ExactTeleportPad_Y,
    ExactTeleportPad_LeftTrigger,
    ExactTeleportPad_RightTrigger,
    ExactTeleportPad_LeftStickUp,
    ExactTeleportPad_LeftStickDown,
    ExactTeleportPad_LeftStickLeft,
    ExactTeleportPad_LeftStickRight,
    ExactTeleportPad_RightStickUp,
    ExactTeleportPad_RightStickDown,
    ExactTeleportPad_RightStickLeft,
    ExactTeleportPad_RightStickRight,
    ExactTeleportPad_COUNT
};
constexpr int kExactTeleportPadKeyBase = 0x1000;
constexpr int kExactTeleportMaxBindableKey = kExactTeleportPadKeyBase + ExactTeleportPad_COUNT - 1;

// Exact-position teleport / force-freefall test plugin.
//
// A rising press of the configured force key:
//   1. (optional) runs a raycast ledge/wall gate around the player, so the
//      force-freefall cannot be triggered in mid-air with no ledge nearby,
//   2. snapshots the player's current transform and position,
//   3. writes that unchanged transform/position back through the game's
//      BaseEntity::SetTransform path,
//   4. (optional) dispatches the same HSTE ResetBehavior used by ACUFixes'
//      debug TeleportPlayer helper so the game re-evaluates the current
//      context (m_ResetBehaviorAfterTeleport; with it off the teleport is a
//      same-value write and should cause NO state change - test mode),
//   5. (optional, OFF by default) requests EnumParkourAction::breakfall
//      through the game's parkour-action request pipeline.
//
// VALIDATED STABLE CONFIGURATION (crash A/B test):
//   ResetBehaviorAfterTeleport=1 + BreakfallAfterTeleport=0
// was used constantly for 30+ minutes with zero crashes. With
// ResetBehaviorAfterTeleport=0 the keypress does nothing at all (the
// transform write is a same-value no-op), which confirms the HSTE reset is
// the mechanism that drops the player into freefall. The breakfall-request
// layer is the identified crash source and is off by default; once in
// freefall, breakfall natively with E instead.
//
// The same-position teleport/reset behavior was confirmed by the tester to put
// the player into freefall when executed mid-air.
class ExactPositionTeleportPlugin {
public:
    void Init();
    void OnUpdate();
    void OnImGuiRender();

    // Called from the game-thread hook (per-frame FOV site 0x141F3FE3B).
    // Executes a queued HSTE reset on the game/simulation thread instead of
    // the Present thread when SafeInjectionOnly is enabled.
    void ServiceDeferredReset();

private:
    void BeginForceFreefall();
    bool TeleportToExactCurrentPosition();
    bool ProbeLedgeGate();
    bool ProbeStateGate();
    void ServiceShiftNegation();
    void ServiceHeldBreakfall();
    void ProcessPendingBreakfall();
    void ReleaseForce();
    void UpdatePadStates();
    void UpdateKeyState(int bindableKeyCode);
    void UpdateAllKeyStates();
    bool WasKeyPressed(int bindableKeyCode) const;
    bool IsForceKeyDown() const;
    static bool IsValidBindableKey(int bindableKeyCode);
    static bool IsPadCode(int bindableKeyCode);

    void LoadSettings();
    void SaveSettings();
    bool EnsureIniFile();
    std::string ResolvePrimaryIniPath() const;

    void SetStatus(const std::string& status);
    void Log(const std::string& line) const;
    std::string GetLogPath() const;

    // key / general
    int  m_ForceKey = 'T';                 // virtual key or 0x1000+ XInput code
    bool m_WaitingForKey = false;
    std::array<bool, kExactTeleportMaxBindableKey + 1> m_PreviousKeyDown {};
    std::array<bool, kExactTeleportMaxBindableKey + 1> m_KeyPressedThisFrame {};
    std::array<bool, kExactTeleportMaxBindableKey + 1> m_KeyStateUpdatedThisFrame {};
    std::array<bool, ExactTeleportPad_COUNT> m_PadDown {};
    int m_ActivePadSlot = -1;
    bool m_PadDllLoaded = false;
    int m_JustCapturedKey = 0;

    // Master toggle ("Enable CatchLedge"): when off the plugin never touches
    // the game - no teleport, no reset, no breakfall requests.
    bool m_PluginEnabled = true;

    // Show the collapsible "Debug / parameters" section in the UI.
    bool m_ShowDebugUi = false;

    // Debug toggle: when off, skips the transform write entirely and only
    // dispatches the HSTE reset (isolates the reset from the teleport).
    bool m_TeleportBeforeReset = true;

    // breakfall
    // OFF by default: the request layer is the identified crash source.
    // VALIDATED STABLE CONFIG: reset ON + this OFF (30+ min constant use, 0 crashes).
    bool m_BreakfallAfterTeleport = false;
    bool m_HoldBreakfallWhileHeld = true;
    int  m_BreakfallActionId = 18;         // EnumParkourAction::breakfall
    int  m_BreakfallDelayMs = 100;         // delay before the first request
    int  m_BreakfallHoldIntervalMs = 0;    // 0 = re-issue every frame while held

    // Safe injection method checkbox: when ON, ignores the external
    // breakfall-request layer (reset-only, no ParkourAction__Request).
    // This is the validated stable configuration (30+ min, 0 crashes).
    bool m_SafeInjectionOnly = false;

    // Buffer between first detection of a valid human state and allowing
    // the teleport/freefall. Prevents rapid transition corruption.
    int  m_StateDetectionBufferMs = 200;

    // When enabled, while ForceKey is held, the teleport/freefall mechanism
    // is re-applied after each ForceCooldownMs (not just the breakfall
    // request). Creates a continuous forced-freefall loop.
    bool m_ReapplyTeleportWhileHeld = false;
    bool m_DeferredResetRequested = false;
    bool m_DeferredResetDone = false;
    bool m_GameThreadHookInstalled = false;
    uint64_t m_DeferredResetAtMs = 0;

    // HSTE reset after the transform write. In the exact-position pipeline
    // this is the only state-changing call; toggling it off turns the
    // teleport into a same-value write (crash A/B test mode).
    bool m_ResetBehaviorAfterTeleport = true;

    // While mid-jump (InSafeJumpOrLoweringToHang), make the game see Shift as
    // released so sprint cannot fight the forced freefall (fixes jitter /
    // walking-in-air). See ServiceShiftNegation().
    bool m_NegateShiftInSafeJump = true;

    // human-state gate
    bool m_StateGateEnabled = true;      // block while in the known-invalid states
    bool m_StateGateRequireValid = false; // also block when in none of the known states

    // cooldown between force-freefall/teleport attempts (ms)
    int  m_ForceCooldownMs = 300;

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
    bool m_ForceActive = false;
    bool m_PendingBreakfall = false;
    bool m_LastTransformApplied = false;
    bool m_LastResetDispatched = false;
    bool m_LastBreakfallRequested = false;
    bool m_ShiftNegationActive = false;
    int  m_HoldRequestCount = 0;
    uint64_t m_BreakfallRequestAt = 0;
    uint64_t m_NextHoldRequestAt = 0;

    bool  m_LastProbeFound = false;
    int   m_LastProbeRays = 0;
    int   m_LastProbeWalls = 0;
    float m_LastProbeDistance = 0.0f;
    float m_LastProbeRise = 0.0f;

    uint64_t m_NextForceAllowedAt = 0;
    uint64_t m_StateValidSinceMs = 0; // when a valid state was first seen

    // last human-state-gate observation (for the ImGui readout)
    bool m_LastStateGateEvaluated = false;
    bool m_LastStateGateBlocked = false;
    bool m_LastStateOnGroundLow = false;
    bool m_LastStateOnGroundHigh = false;
    bool m_LastStateOnPeakOrLedge = false;
    bool m_LastStateWalling = false;
    bool m_LastStateSafeJumpOrLowering = false;
    bool m_LastStateWallrunUp = false;

    float m_LastX = 0.0f;
    float m_LastY = 0.0f;
    float m_LastZ = 0.0f;
    std::string m_IniPath;
    std::string m_LastStatus = "Not initialized";
};
