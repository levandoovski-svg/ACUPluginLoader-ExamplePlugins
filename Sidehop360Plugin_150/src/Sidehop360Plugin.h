#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

// Standalone plugin for ACU 1.5.0 (ACUFixes-0.9.3 address set): gives the
// sidehop a 360-degree range of execution while walling, behind a hold-hotkey.
//
// Two togglable mechanisms (both gated by the same hold-hotkey):
//
//   Hook A - angle widen (the base mechanism).
//     The sidehop's acceptance cone is `minCosAngle` (+0x1E8 of
//     AvailableParkourAction), written to cos45 = 0.7071f by the
//     set-ranges impl 0x140148500 (fancy vtable 0x143A13130 slot 0x64)
//     at 0x140148549. While the hotkey is held, the Hook A cave overrides
//     it (default -1 = full 360 degrees) before the fit chain consumes it.
//
//   Hook B - position nudge (the tunable extra, breakfall-style).
//     Mirrors the WallToHang extra-call pattern: at the climbFacade scan's
//     call site 0x140135230 the callback copies the probe point, nudges it
//     in the ground plane, and asks CreateParkourActionForParkourPointIfFits
//     (0x1401D1CF0) to create ONE extra sidehop from the nudged point. The
//     vanilla call still runs right after via the stolen bytes. Refuses to
//     install when another plugin (e.g. ACU_ParkourPatches) owns the site.
class Sidehop360Plugin
{
public:
    void OnBeforeActivate();
    void OnUpdate();
    void OnImGuiRender();

    // Hook A gate: hotkey held (and not being rebound).
    bool ShouldWidenAngle() const;
    // Hook B gate: enabled + hotkey held.
    bool ShouldNudge() const;

private:
    void LoadSettings();
    void SaveSettings();
    static std::string GetIniPath();

public:
    bool m_Enabled = true;

    // Hook A. -1 = accept from any angle (360 degrees); vanilla is 0.7071 (cos 45).
    float m_MinCosAngle = -1.0f;
    int   m_Hotkey = VK_F8;          // hold
    bool  m_WaitingForHotkey = false;
    bool  m_CaptureArmed = false;    // the click that started the capture must not capture itself

    // Hook A additions (test-1 findings: the +-45 limit lives at candidate
    // CREATION, not in the fit cone, and the sidehop expects a 2-3 m hop -
    // so widening the cone alone changes nothing. These re-aim/loosen the
    // existing candidates instead).
    // Redirect re-writes the candidate's target in the ground plane:
    //   0 = off (only widen/loosen), 1 = mirror 180 deg,
    //   2 = off the wall (along the wall's outward normal, HopDistance meters).
    int   m_RedirectMode = 0;
    float m_HopDistance = 2.5f;      // meters, mode 2 target distance
    float m_ExpectedSideMin = 0.5f;  // vanilla writes 2.0 (+0x238)
    float m_ExpectedSideMax = 8.0f;  // vanilla writes 3.0 (+0x23C)

    // Hook B.
    bool  m_NudgeEnabled = false;    // default off: tunable extra, site usually owned by ACU_ParkourPatches
    int   m_NudgeDirectionMode = 0;  // 0 = opposite of the movement input, 1 = along it, 2 = off the wall
    float m_NudgeDistance = 0.5f;    // meters

    // Wall's outward normal (XY, normalized), cached by the Hook A cave from
    // directionDestFacingOut (+0x40) of the most recent candidate. Hook A runs
    // on every candidate creation, so this stays fresh while walling. Used by
    // redirect mode 2 and nudge direction mode 2.
    float m_WallNormalX = 0.0f;
    float m_WallNormalY = 0.0f;
    bool  m_HasWallNormal = false;

    // Hook states (each hook installs independently of the other).
    bool m_HooksInstalled = false;
    bool m_HookAInstalled = false;
    bool m_HookBInstalled = false;
    bool m_HookARefused = false;     // 0x140148553 patched by someone else (unexpected)
    bool m_HookBRefused = false;     // 0x140135230 E9-patched (expected with ACU_ParkourPatches)

    // Telemetry.
    long long m_TelWidenApplications = 0;
    float     m_TelLastMinCosAngleSeen = 0.0f; // what vanilla wrote before our override
    long long m_TelNudgeCalls = 0;
    long long m_TelNudgeCreated = 0;
    long long m_TelRedirects = 0;
};
