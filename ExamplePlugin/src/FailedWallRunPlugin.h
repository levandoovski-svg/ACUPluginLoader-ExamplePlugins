#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"

#include <memory>

class AvailableParkourAction;

// ── Hook 1: creation injection ────────────────────────────────────────────────
// Mid-facade-scan hook (same 12-byte VM-thunk site ACUFixes/ACU_ParkourPatches use)
// that creates an extra wallrunUpFromGroundFailed_mb (52) action and appends it
// to the candidate array.
struct GPH_FailedWallrunCreator : AutoAssemblerCodeHolder_Base
{
    GPH_FailedWallrunCreator();
};

// ── Hook 2: range widener ─────────────────────────────────────────────────────
// Runs INSIDE wallrunUpFromGroundFailed_mb's
// SetExpectedDistanceRangesAccordingToParkourMode (fancy idx 0x64 == 0x1401AF2E0),
// i.e. AFTER the game has written its vanilla acceptance ranges and BEFORE the
// per-type geometry test (fancy idx 0x4A) and CalculateFitness consume them.
// This is the same "patch the ranges during creation" pattern ACU_ParkourPatches
// uses for wallEjectToHang (its hooks at 0x140140E44 / 0x140140DCA).
//
// Fix for the old plugin's ordering bug: ranges used to be widened only AFTER
// CreateParkourActionForParkourPointIfFits had already run InitialTestIfActionFits
// and destroyed the action on non-qualifying walls.
struct GPH_Type52RangeWidener : AutoAssemblerCodeHolder_Base
{
    GPH_Type52RangeWidener();
};

// ── Hook 3: per-type fit-test override ────────────────────────────────────────
// The type-52 "does this wall qualify" verdict is produced by
//   InitialTestIfActionFits (0x1401B4900, fancy idx 0x4B)
//     └─ per-type test fancy idx 0x4A (0x1401B0340)
//          └─ 0x1401B5700 = 5 per-context quick-checks (parkour mode, wall slope,
//             mesh, ...) = the actual "wall context" gate.
// At 0x1401B499E the shared wrapper does `TEST AL,AL / JZ <reject>` right after
// that per-type test returns. This 4-byte hook re-executes those exact stolen
// bytes; the callback just forces AL=1 for type-52 actions while the feature is
// active (other action types pass through untouched — gated on the action's
// fancy vtable being the type-52 runtime table 0x143A6FFD0).
struct GPH_Type52FitTestOverride : AutoAssemblerCodeHolder_Base
{
    GPH_Type52FitTestOverride();
};

// ── Hook 4 (optional): cycle-start tester vtable swap ─────────────────────────
// Port of ACUFixes-0.9.3 / ACU_ParkourPatches "WhenStartingWallingCycle /
// WhenStartingFailedWallrunCycle_ChangeFVTables": while active, swaps the
// climbFacadeTester fancy vtable (default 0x1439E08D0 ↔ baseclass
// 0x1439FA1A0) at walling/failed-wallrun cycle start so more move types get
// scanned in wall contexts. Both 7-byte sites are Denuvo VM thunks verified in
// the 1.5.0 runtime dump, and both addresses are proven on this build by the
// (working) third-party ACU_ParkourPatches.dll.
struct GPH_CycleStartVTableSwap : AutoAssemblerCodeHolder_Base
{
    GPH_CycleStartVTableSwap();
};

class FailedWallRunPlugin
{
public:
    FailedWallRunPlugin();
    ~FailedWallRunPlugin();

    void OnUpdate();
    void OnImGuiRender();
    void LoadSettings();
    void SaveSettings();

    // Run on the game thread inside the parkour sort&select stage (via
    // GenericHooksInParkourFiltering). Returning an action force-selects it.
    AvailableParkourAction* ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions);
    AvailableParkourAction* ChooseAfterSorting(SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame);

private:
    static bool IsRisingEdgePressed(int vkCode);

    bool IsFeatureActive() const;
    bool IsInParkourState() const;

    static constexpr uint32 kParkourRTCPIdx = 372;
    static constexpr uint32 kGeneralStateRTCPIdx = 237;

    AutoAssembleWrapper<GPH_FailedWallrunCreator> m_FailedWallrunCreator;
    AutoAssembleWrapper<GPH_Type52RangeWidener> m_RangeWidener;
    AutoAssembleWrapper<GPH_Type52FitTestOverride> m_FitTestOverride;
    AutoAssembleWrapper<GPH_CycleStartVTableSwap> m_VTableSwapHook;

    bool m_CreatorHookActive = false;
    bool m_WidenerHookActive = false;
    bool m_FitTestHookActive = false;
    bool m_VTableSwapHookActive = false;

    // Sort&select enforcement (same mechanism FreeJump/BlackFlagGrab use).
    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_HookActivator;
    bool m_GphActive = false;

    bool m_PluginEnabled = true;
    int m_ToggleKey = 0x76; // F7
    bool m_WaitingForKey = false;
    bool m_HoldMode = true;
    bool m_ToggleState = false;

    float m_LiftAmount = 2.0f;
    float m_LiftSpeed = 6.0f;

    bool m_EnforceEnabled = true;
    bool m_RangeWidenerEnabled = true;
    bool m_SkipFitTestEnabled = true;
    bool m_VtableSwapEnabled = false;

    bool m_CancelEnabled = false;

    // Debug stats (written from the game thread, read by the UI).
    int m_LastAction52Count = 0;
    int m_LastSabotagedCount = 0;
};
