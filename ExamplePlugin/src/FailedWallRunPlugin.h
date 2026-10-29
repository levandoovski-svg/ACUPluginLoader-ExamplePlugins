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

// ── Hook 2 (REMOVED): range widener as a code hook ───────────────────────────
// The old hook sat INSIDE 0x1401AF2E0, which binary analysis proved is type-54's
// (wallrunUpToHang) range-setter -- NOT type 52's (0x14019AA50, slot 0x143A52F80).
// It polluted wallrunUpToHang and never touched type 52. Widening now happens
// inside the fit-test stub (Hook 3): the swapped 0x4A slot runs strictly after
// 0x64 wrote the vanilla ranges and before the per-type verdict consumes them,
// so the ordering guarantee holds with no mid-function code hook.

// ── Hook 3: per-type fit-test override (fancy-table slot swap) ────────────────
// The "does this wall qualify" verdict is the fancy-0x4A call inside
// InitialTestIfActionFits (0x1401B4900). Two slot addresses are swapped:
//   * 0x143A52C40 -- type-52 RUNTIME table (0x143A522F0, dump-verified: its
//     0x45 accessor returns 0x34 = 52) 0x4A, vanilla 0x14019B6F0;
//   * 0x143A42280 -- TRANSIENT base table (0x143A41930) 0x4A, vanilla
//     0x1401B5700. SHARED by 22 tables, so the stub widens/overrides ONLY
//     actions whose table it recognizes, forwarding everything else untouched.
// The swapped stub (a) widens the action's acceptance ranges (only loosens;
// runs after 0x64 wrote vanilla ranges, before the verdict/CalculateFitness
// consume them) and (b) optionally returns true, skipping the wall-context gate.
// The previous implementation patched 0x143A70920, which is type-54's 0x4A
// (vanilla 0x1401B0340) -- wrong table, wrong vanilla value, wrong effect.
// (Implemented as plain function-pointer swaps in FailedWallRunPlugin.cpp --
// no AutoAssembler hook needed.)

// ── Hook 4 (optional): cycle-start tester vtable swap ─────────────────────────
// Port of ACUFixes-0.9.3 / ACU_ParkourPatches "WhenStartingWallingCycle /
// WhenStartingFailedWallrunCycle_ChangeFVTables": while active, swaps the
// climbFacadeTester fancy vtable (default 0x1439E08D0 <-> baseclass
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

    // Runs on the game thread inside the creation-stage replacements, for EVERY
    // constructed action, BEFORE the discard decision (requires
    // RequestGPHCreation). Return true to keep an action the vanilla fit test
    // rejected; the action may be mutated in place first. This is the
    // mechanism that keeps wallrunUpFromGroundFailed_mb (52) alive on
    // non-qualifying walls and converts wallrunUpFromGround_mb (53) into it.
    bool OverrideCreationDiscard(AvailableParkourAction& action);

private:
    static bool IsRisingEdgePressed(int vkCode);

    bool IsFeatureActive() const;
    bool IsInParkourState() const;

    static constexpr uint32 kParkourRTCPIdx = 372;
    static constexpr uint32 kGeneralStateRTCPIdx = 237;

    AutoAssembleWrapper<GPH_FailedWallrunCreator> m_FailedWallrunCreator;
    AutoAssembleWrapper<GPH_CycleStartVTableSwap> m_VTableSwapHook;

    bool m_CreatorHookActive = false;
    bool m_FitTestHookActive = false;
    bool m_VTableSwapHookActive = false;

    // Vanilla-byte verification results (fail-closed guards; shown in the UI).
    bool m_CreatorSiteOk = true;
    bool m_FitTestSlotOk = true;
    bool m_SwapSitesOk = true;

    // Sort&select enforcement (same mechanism FreeJump/BlackFlagGrab use).
    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_HookActivator;
    std::shared_ptr<SharedHookActivator> m_HookActivatorCreation;
    bool m_GphActive = false;

    // Captured wallrunUpFromGroundFailed_mb (52) RUNTIME fancy vtable, used to
    // convert wallrunUpFromGround_mb (53) into it at creation time (same
    // behavior-swap pattern as ForcedSidehop). 0 = not captured yet; captured
    // only from the dump-verified runtime table, never from the transient base
    // table. Approach any wall once so a natural type-52 gets created.
    static uint64 s_Type52FancyVTable;

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

    // Toggleable single-stage mode that makes this plugin work EXACTLY like
    // ForcedSidehopPlugin (rewrites existing array candidate, skips creation/
    // range-widener/fit-override/vtable hooks). Set via checkbox in UI.
    bool m_SingleStageMode = false;

    // Debug stats (written from the game thread, read by the UI).
    int m_LastAction52Count = 0;
    int m_LastSabotagedCount = 0;
};
