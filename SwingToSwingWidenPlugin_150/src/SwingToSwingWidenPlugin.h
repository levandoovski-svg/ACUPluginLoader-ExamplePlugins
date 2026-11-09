#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

class AvailableParkourAction;

// SwingToSwingWidenPlugin_150 - applies the "angle widen" mechanism (proven on
// the sidehop, see MODLOG/sidehop-angle-widen.md) to the swing-to-swing move
// (EnumParkourAction::swing_31_swingToSwing_mb = 49) on ACU 1.5.0.
//
// WHY A FANCY-VTABLE SLOT SWAP INSTEAD OF CODE CAVES (Freeform 2.0 compatibility)
// ------------------------------------------------------------------------------
// The sidehop plugin had to patch two game code sites: the set-ranges impl
// (0x140148553) and QuickCheckAcceptableDistanceRanges (0x1401B5890).
// For the swing-to-swing that second site is UNUSABLE as a code hook:
//   0x1401B5890 is shared by 118 action types (it is the generic range check),
//   so patching its bytes would (a) collide with Sidehop360's own Hook C cave,
//   and (b) change acceptance for every other parkour move unless filtered.
// The swing's own set-ranges (0x1401B60A0) is type-exclusive, but it splits into
// per-parkour-mode branches with three separate `minCosAngle` writes and two
// different `ret`s - no single clean seam exists after all of them.
//
// So this plugin touches NO game code bytes at all. It repoints ONE slot of the
// swing-to-swing's own fancy vtable (0x143A77E90, slot 0x70 =
// QuickCheckAcceptableDistanceRanges) to a thunk that applies the overrides and
// then calls the original 0x1401B5890. The table lives in the writable .data
// section, so the swap is a single 8-byte pointer write, restored when the mode
// is turned off.
//
// That makes it compatible with:
//   * Freeform 2.0 / FreeJumpPlugin - it hooks the climbFacade scan call site
//     0x140135230 and the sort&select 0x140133B00; this plugin never touches
//     those, and never calls into them.
//   * Sidehop360Plugin_150 - its Hook C still owns the bytes of 0x1401B5890;
//     our thunk calls that very address, so both mechanisms stack instead of
//     fighting over a patch site.
//   * ACUFixes parkour helpers - same reasoning (no shared sites).
//
// VERIFIED 1.5.0 FACTS (dump MODLOG/scripts/ACU_dumped.bin, base 0x140000000)
// ---------------------------------------------------------------------------
//   fancy vtable of type 49          0x143A77E90 (in .data, RW)
//     slot 0x45 GetEnumParkourAction 0x1401B59F0 (mov eax,0x31; ret)
//     slot 0x64 SetExpectedDistanceRanges... 0x1401B60A0 (type-exclusive)
//     slot 0x70 QuickCheckAcceptableDistanceRanges 0x1401B5890 (SHARED)
//   The 9-byte entry of 0x1401B5890 is `push rbx; sub rsp,0x20; mov rbx,rcx`.
//
// What the shared check does (0x1401B5890), with e = epsilon (+0x1FC):
//   mode 5                 -> accepted unconditionally
//   heightDifference (+0xC0)     in [ +0x1D0 - e , +0x1D4 + e ]   ABSOLUTE bounds
//   horizontalDifference (+0xC4) in [ +0x1D8 - e , +0x1DC + e ]
//   distance (+0xC8)             in [ +0x1E0 - e , +0x1E4 + e ]
//   cone: minCosAngle (+0x1E8) - epsilonForDotProd (+0x200) <= flt_CC (+0xCC)
//   then 0x1401CBCD0, a +0x1F4/+0xD4 check, and fancy slot 0x71.
// NOTE: +0x1D4/+0x1DC/+0x1E4 are ABSOLUTE upper bounds, not offsets from the
// mins (this corrects the earlier Sidehop360 write-up; verified from the
// comparison order: min-e <= value, value <= e+max).
//
// Vanilla swing-to-swing values written by 0x1401B60A0:
//   always:            +0x1D8=1.0, +0x1DC=12.0, +0x1E0=1.0, +0x1E4=12.0,
//                      +0x23C=2.5, +0x238=0, +0x1F8=0.05
//   mode 1 (ParkourUp): minCosAngle=0x3F5DB3D7 (cos30), height -2.0 .. +1.0
//   mode 2 (Straight):  minCosAngle=0x3F3504F3 (cos45), height -1.0 .. 0
//   mode 3 (Down):      minCosAngle=0x3F7746EA (cos15), height -12.0 .. 0
//   mode 0/4+:          minCosAngle NOT written (mode 0 only gets the base set)
// That per-mode variety is why the check-time override is the right place: one
// seam covers every mode with one set of numbers.
class SwingToSwingWidenPlugin
{
public:
    void OnBeforeActivate();
    void OnUpdate();
    void OnImGuiRender();

    // Hotkey held (and not being rebound) and the mode is on.
    bool ShouldWiden() const;

    // Called from the table-slot thunk for every swing-to-swing candidate check.
    void ApplyOverrides(AvailableParkourAction& action);

    // Fancy-vtable slot swap (no code patching).
    bool TryInstallSlotSwap();
    void RemoveSlotSwap();
    bool IsSlotAllowed() const { return m_Mode != 0; }

private:
    void LoadSettings();
    void SaveSettings();
    static std::string GetIniPath();

public:
    // 0 = off, 1 = angle widen.
    int m_Mode = 0;

    int m_Hotkey = VK_F9;            // hold; F9 so it does not fight Sidehop360's F8
    bool  m_WaitingForHotkey = false;
    bool  m_CaptureArmed = false;    // the click that started the capture must not capture itself

    float m_MinCosAngle = -1.0f;     // -1 = accept any direction; vanilla is 0.7071-0.966 depending on the mode
    float m_HeightDiffMin = -10.0f;  // allowed dest-height offset below src (+0x1D0, absolute)
    float m_HeightDiffMax = 10.0f;   // allowed dest-height offset above src (+0x1D4, absolute)

    // Slot swap state.
    bool m_SlotInstalled = false;
    bool m_SlotRefused = false;
    std::string m_RefusalReason;

    // Telemetry.
    long long m_TelChecksSeen = 0;
    long long m_TelOverridesApplied = 0;
    long long m_TelSlotInstalls = 0;
    float     m_TelLastMinCosAngleSeen = 0.0f;
};
