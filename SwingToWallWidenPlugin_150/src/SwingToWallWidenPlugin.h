#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

class AvailableParkourAction;

// SwingToWallWidenPlugin_150 - applies the "angle widen" mechanism (see
// MODLOG/sidehop-angle-widen.md) to the two swing-to-wall moves on ACU 1.5.0:
//
//   46 = swingToWalling_front          "swing into the wall in front of you"
//   43 = (not touched) swing_2B
//   47 = swing_2F_SwingToSidewall_includingSpindescent
//                                      "swing onto the wall at your side
//                                       (also used by the spindescent)"
//
// Both moves are toggled independently in the panel; the widen settings (cone
// and allowed height window) are shared. A hold-hotkey gates the overrides.
//
// MECHANISM - FANCY-VTABLE SLOT SWAP, NO CODE PATCHING
// ----------------------------------------------------
// Both moves use QuickCheckAcceptableDistanceRanges = 0x1401B5890, the generic
// range check shared by 118 action types, so a byte hook there would leak to
// every other parkour move and would collide with Sidehop360's Hook C cave.
// Their own set-ranges functions are type-exclusive but branch per parkour mode
// (no single post-write seam). This plugin therefore repoints slot 0x70 of each
// move's own fancy vtable(s) to a thunk that applies the overrides and then
// calls the original 0x1401B5890.
//
// No game code bytes are modified, so it coexists with Freeform 2.0
// (0x140135230 / 0x140133B00), Sidehop360 (cave at 0x1401B5890) and ACUFixes
// parkour helpers. Slots are restored when a move is disabled.
//
// VERIFIED 1.5.0 TABLES (dump MODLOG/scripts/ACU_dumped.bin, base 0x140000000)
// ---------------------------------------------------------------------------
// Type 46 has THREE fancy vtables (different creation contexts); all three are
// widened. Type 47 has one.
//
//   table       getenum (0x45)  set-ranges (0x64)  check (0x70)
//   0x1439E6120 0x1401A7B10     0x140145C30         0x1401B5890
//   0x143A0B080 0x1401A7B10     0x140145C30         0x1401B5890
//   0x143A66D20 0x1401A7B10     0x1401A87D0         0x1401B5890
//   0x143A3E170 0x1401909E0     0x140190DC0         0x1401B5890
//
// Vanilla cones written by those set-ranges functions:
//   type 46: cos15 (0x3F7746EA) in the shared prologue; the per-mode bodies of
//            0x1401A87D0 then use cos45 / cos30 / cos15 depending on the mode.
//   type 47: cos75 (0x3E8483EE) - already a very wide cone; widening still
//            removes the remaining rejections and the range windows.
// The +0x1D0/+0x1D4 height windows are ABSOLUTE bounds, exactly like the
// swing-to-swing and sidehop cases (the check does `value <= e + max`).
class SwingToWallWidenPlugin
{
public:
    void OnBeforeActivate();
    void OnUpdate();
    void OnImGuiRender();

    // Hotkey held (and not being rebound), mode on, and the calling move enabled.
    bool ShouldWiden() const;

    // 46 or 47 - identified from the candidate's fancy-vtable pointer. 0 = unknown.
    int IdentifyMove(const AvailableParkourAction* action) const;

    void ApplyOverrides(AvailableParkourAction& action, int moveId);

    bool TryInstallSlot(int targetIndex);
    void RemoveSlot(int targetIndex);

private:
    void LoadSettings();
    void SaveSettings();
    static std::string GetIniPath();
    bool IsMoveEnabled(int moveId) const;

public:
    // 0 = off, 1 = angle widen.
    int m_Mode = 0;

    int m_Hotkey = VK_F10;           // hold; F8 is Sidehop360's, F9 is swing-to-swing's
    bool  m_WaitingForHotkey = false;
    bool  m_CaptureArmed = false;    // the click that started the capture must not capture itself

    bool  m_WidenFrontWall = true;   // type 46 (3 tables)
    bool  m_WidenSideWall = true;    // type 47 (1 table)

    float m_MinCosAngle = -1.0f;     // -1 = accept any direction
    float m_HeightDiffMin = -10.0f;  // allowed dest-height offset below src (+0x1D0, absolute)
    float m_HeightDiffMax = 10.0f;   // above src (+0x1D4, absolute too)

    // Per-table slot state (see the table list in the .cpp).
    static constexpr int k_TargetCount = 4;
    bool  m_SlotInstalled[k_TargetCount] = {};
    bool  m_SlotRefused[k_TargetCount] = {};
    std::string m_RefusalReason[k_TargetCount];

    // Telemetry.
    long long m_TelChecks46 = 0;
    long long m_TelChecks47 = 0;
    long long m_TelOverrides46 = 0;
    long long m_TelOverrides47 = 0;
    long long m_TelSlotInstalls = 0;
    float     m_TelLastMinCosAngleSeen = 0.0f;
};
