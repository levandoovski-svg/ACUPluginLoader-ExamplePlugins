#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

// Standalone plugin for ACU 1.5.0 (ACUFixes-0.9.3 address set): gives the
// sidehop a 360-degree range of execution while walling, behind a hold-hotkey.
//
// v0.3.0: ONE "Mode" selector - the mechanisms are mutually exclusive so each
// can be tested in isolation (nudge its own mode, the main angle-widen
// toggleable by picking another mode):
//
//   Mode 1 "Angle widen" (the main) = Hook A + Hook C.
//     Hook A (0x140148553, inside the sidehop's set-ranges impl) overrides
//     minCosAngle (+0x1E8, vanilla cos45) and the expected-hop range
//     Side_min/Max (+0x238/+0x23C, vanilla 2/3 m).
//     Hook C (0x1401B5890 = QuickCheckAcceptableDistanceRanges of the
//     sidehop's fancy vtable, entry cave) rewrites the ALLOWED ranges right
//     before the checks consume them: heightDifferenceMin/Max (+0x1D0/+0x1D4),
//     horizontalDifferenceMin/Max (+0x1D8/+0x1DC), distanceMin/Max
//     (+0x1E0/+0x1E4). Needed because set-ranges writes heightDifferenceMin/
//     Max per parkour mode AFTER Hook A's site (sidehop: -1.0..+1.5 m), which
//     no Hook A override can survive - that un-overridden height window was
//     the remaining gate (test-2 research).
//
//   Mode 2 "Redirect" = Hook A + Hook C, plus re-aiming every sidehop
//     candidate's target in the ground plane (sub-mode: mirror 180 degrees,
//     or off the wall along the wall normal, HopDistance meters). ALL
//     direction fields are rewritten for consistency (dest anchor, dir80,
//     directionSrcToDest + both XYPlane variants) plus horizontalDifference/
//     distance, so the candidate is internally coherent when QuickCheck and
//     the geometry tests see it.
//
//   Mode 3 "Nudge" = Hook B (0x140135230, the climbFacade scan's call to
//     CreateParkourActionForParkourPointIfFits) adds ONE extra sidehop
//     created from a nudged copy of the probe point. The site is owned by
//     Freeform 2.0, which hooks the same call site (live-verified: the E9
//     cave dispatches into Freeform 2.0.dll; the "ACU_dumped.bin" bytes at
//     this site are Freeform's hook, NOT vanilla - the dump was captured
//     with Freeform running). Hook B refuses while taken and retries every
//     pass - disabling Freeform 2.0 in the loader frees the site and the
//     hook installs on its own. Do not re-enable Freeform 2.0 while the
//     nudge is installed.
//     Hook A also installs in Mode 3, cache-only: it caches the wall's
//     outward normal for the nudge's "off the wall" direction.
//
//   Mode 4 "Aim (creation)" = Hook D + Hook A (cache-only) + Hook C.
//     Hook D (0x1401584B0, the type-39 action CONSTRUCTOR - vanilla, one
//     caller: the type-39 case of the creation factory) intercepts every
//     sidehop construction. While the hotkey is held it substitutes the probe
//     point (r8) with a copy shifted along the player's movement input by
//     AimDistance, then lets the vanilla ctor build the action from that point
//     (the pointer is swapped, so the scanner's own buffers are never
//     mutated). This is the first mechanism that acts at CREATION stage - the
//     stage the widen experiments proved is the real +-45-degree gate - so it
//     is the path toward omnidirectional targeting.
//     The wrapper 0x1401D1CF0 that v0.4.0 tried to hook is owned by
//     FreeFormWallrun.dll (live-verified: FF 25 + pointer to +0xAB90), and that
//     plugin reimplements the wrapper; the ctor is untouched and is still
//     reached through the factory, so this hook works with Freeform loaded.
//     Hook C stays installed so the shifted point can pass the range check.
//     Telemetry separates the failure modes: "constructions seen" (ctor calls),
//     "points aimed" (Hook D actually substituted) and "reaching the sidehop
//     check" (sidehop-only Hook C count; if it stays 0 the shifted point was
//     rejected inside the ctor, before any range gate).
class Sidehop360Plugin
{
public:
    void OnBeforeActivate();
    void OnUpdate();
    void OnImGuiRender();

    // Hotkey held (and not being rebound) - gates all three hooks.
    bool ShouldWidenAngle() const;

private:
    void LoadSettings();
    void SaveSettings();
    static std::string GetIniPath();

public:
    // 0 = off, 1 = angle widen (main), 2 = redirect, 3 = nudge, 4 = aim (creation).
    int m_Mode = 0;

    int m_Hotkey = VK_F8;            // hold
    bool  m_WaitingForHotkey = false;
    bool  m_CaptureArmed = false;    // the click that started the capture must not capture itself

    // Modes 1-2 (Hook A / Hook C).
    float m_MinCosAngle = -1.0f;     // -1 = accept from any angle; vanilla cos45 = 0.7071
    float m_ExpectedSideMin = 0.5f;  // vanilla writes 2.0 (+0x238)
    float m_ExpectedSideMax = 8.0f;  // vanilla writes 3.0 (+0x23C)
    float m_HeightDiffMin = -10.0f;  // allowed dest height offset below src (+0x1D0, absolute)
    float m_HeightDiffMax = 10.0f;   // above src (+0x1D4, absolute too - the check does e+max)

    // Mode 2 (redirect). Sub-mode: 1 = mirror 180 deg, 2 = off the wall.
    int   m_RedirectSubMode = 2;
    float m_HopDistance = 2.5f;      // meters, off-the-wall target distance

    // Mode 3 (nudge).
    int   m_NudgeDirectionMode = 0;  // 0 = opposite of movement input, 1 = along it, 2 = off the wall
    float m_NudgeDistance = 0.5f;    // meters

    // Mode 4 (aim by creation).
    float m_AimDistance = 2.5f;      // meters the probe point is pushed along the movement input

    // Hook states (each hook installs independently).
    bool m_HooksInstalled = false;
    bool m_HookAInstalled = false;
    bool m_HookCInstalled = false;
    bool m_HookBInstalled = false;
    bool m_HookDInstalled = false;
    bool m_HookARefused = false;     // 0x140148553 patched by someone else (unexpected)
    bool m_HookCRefused = false;     // 0x1401B5890 patched by someone else (unexpected)
    bool m_HookBRefused = false;     // 0x140135230 E9-patched by ACUFixes' Parkour helpers (expected)
    bool m_HookDRefused = false;     // 0x1401D1CF0 patched by someone else (unexpected)

    // Wall's outward normal (XY, normalized), cached by the Hook A cave from
    // directionDestFacingOut (+0x40) of the most recent candidate. Hook A runs
    // on every candidate creation, so this stays fresh while walling. Used by
    // redirect sub-mode 2 and nudge direction mode 2.
    float m_WallNormalX = 0.0f;
    float m_WallNormalY = 0.0f;
    bool  m_HasWallNormal = false;

    // Telemetry.
    long long m_TelWidenApplications = 0;
    float     m_TelLastMinCosAngleSeen = 0.0f; // what vanilla wrote before our override
    long long m_TelQuickCheckOverrides = 0;
    long long m_TelRedirects = 0;
    long long m_TelNudgeCalls = 0;
    long long m_TelNudgeCreated = 0;
    long long m_TelAimCreateCalls = 0;  // every type-39 construction attempt seen at Hook D
    long long m_TelAimRewrites = 0;     // probe points actually substituted
    long long m_TelSidehopCheckOverrides = 0; // Hook C overrides on the SIDEHOP only
                                              // (the check impl is shared by 118 types;
                                              // m_TelQuickCheckOverrides counts them all)
};
