#include "pch.h"
#include "FailedWallRunPlugin.h"
#include "ACU/basic_types.h"
#include "ACU/Entity.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/Memory/ACUAllocs.h"
#include "ACU_DefineNativeFunction.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ACU/Parkour_WhileOnWallSystem.h"
#include <fstream>
#include <shlobj.h>
#include "imgui/imgui.h"
#include <cstring>
#include <string_view>
#include <algorithm>

#include "ACU/AtomGraph.h"
#include "ACU/AtomAnimComponent.h"
#include "ACU/HumanStatesHolder.h"

// ============================================================================
//  Failure analysis this rewrite is based on (1.5.0 Steam runtime dump,
//  MODLOG/scripts/ACU_dumped_pe.exe, base 0x140000000, no ASLR):
//
//  0) WRONG-VTABLE BUG (root cause of every previous failure, fixed here).
//     Everything below previously assumed the type-52 runtime fancy vtable was
//     0x143A6FFD0. That table is NOT type 52 -- its fancy idx 0x45
//     (GetEnumParkourAction, 0x1401AD4B0) is a two-instruction accessor that
//     returns 0x36 (54). Binary-derived table map:
//       type 52 wallrunUpFromGroundFailed_mb -> 0x143A522F0 (0x45 = 0x140199500 -> 0x34)
//       type 53 wallrunUpFromGround_mb        -> 0x143A58280 (0x45 = 0x14019C060 -> 0x35)
//       type 54 wallrunUpToHang               -> 0x143A6FFD0 (0x45 = 0x1401AD4B0 -> 0x36)
//     Consequences of the wrong pointer: the 53->52 vtable swap produced a
//     wallrunUpToHang-typed action (so nothing ever matched type 52 and the
//     force-select never fired, the array fell back to vanilla picks which
//     looked like an unrelated move firing), the fit-test slot swap was
//     installed into type 54's table, and the range-widener patch sat inside
//     type 54's range-setter and polluted wallrunUpToHang actions instead.
//     Cross-checks that validate the table map: 0x143A13130 (the vtable
//     ForcedSidehopPlugin successfully swaps to) maps to type 39
//     (climbFacade_fromWallDescentSidewaysToGround_alsoSidehop), i.e. a
//     sidehop; and 0x1439F41C0 maps to type 73 (rise-on-ledge), matching
//     ACUFixes' rise-on-ledge handling.
//
//  1) Ordering bug (the main one): the old plugin widened the type-52
//     acceptance ranges only AFTER CreateParkourActionForParkourPointIfFits
//     (0x1401D1CF0) had already run InitialTestIfActionFits (0x1401B4900)
//     and destroyed the action. On walls where vanilla's ranges reject the
//     probe the wrapper returned false and the plugin did nothing.
//     -> the type-52 per-type fit-test (fancy 0x4A) slot is swapped with a
//        stub that widens the action's ranges and then returns true. This runs
//        strictly after 0x64 wrote the vanilla ranges and strictly before 0x4A
//        consumes them, so the widening always applies in time and no
//        mid-function code hook is needed (the previous 0x1401AF3B6 hook sat
//        inside type 54's range-setter and is removed).
//  2) Null fancy-fn crash: the type-52 ctor chain passes through fancy table
//     0x143A41930 whose idx 0x45 (GetEnumParkourAction) is NULL. Any plugin
//     enumerating actions via GetEnumParkourAction() calls address 0 and
//     crashes. -> fixed by GetTypeIntSafe() (never dispatches a null fn).
//  3) Weak selection: fitness sabotage at creation time only.
//     -> added GPH ChooseBeforeFiltering/ChooseAfterSorting enforcement
//        (the same mechanism the reverse-engineered ACU_ParkourPatches.dll
//        uses when it returns the mutated candidate from sort&select).
//  4) "Nothing happens" / unrelated move fires: the structural causes are the
//     discarded-at-creation 52 (fixed by OverrideCreationDiscard) and the
//     wrong-vtable constant (fixed by note 0).
//
//  Type-52 chain (all dump-verified):
//    creation wrapper 0x1401D1CF0
//      -> factory 0x1401CC990 (jump table 0x1401D1AF4[0x34] -> ctor 0x1401BC7F0)
//      -> fancy 0x3D Set2Floats
//      -> fancy 0x4B InitialTestIfActionFits = 0x1401B4900
//           -> fancy 0x64 SetExpectedDistanceRangesAccordingToParkourMode
//              (type 52 = 0x14019AA50, slot 0x143A52F80; type 54 =
//               0x1401AF2E0, slot 0x143A70C60 -- NOT patched here)
//           -> fancy 0x49 = 0x1401ACBD0 (shared)
//           -> fancy 0x4A per-type test. Type 52 = 0x14019B6F0 (slot
//              0x143A52C40). The transient base table's 0x4A (slot
//              0x143A42280) holds 0x1401B5700, a SHARED quick-check used by 22
//              tables -- so both 0x4A paths are swapped, see below.
//           -> the fit-test VERDICT is overridden from inside the swapped
//              stub: it widens the action's ranges (safe here -- 0x64 already
//              ran, 0x4A is what consumes them) and returns true.
// ============================================================================

// Extended action base with padding matching the game's full action size
class ParkourAction_Commonbase : public AvailableParkourAction
{
public:
    char pad_0290[0x2B0 - 0x290];
};
assert_sizeof(ParkourAction_Commonbase, 0x2B0);

DEFINE_GAME_FUNCTION(CreateParkourActionForParkourPointIfFits, 0x1401D1CF0, char, __fastcall,
    (EnumParkourAction a1, uint64 a2, __m128* a3, __m128* p_movementVecWorld_mb, float a5, int a6, char a7, uint64 a8, uint64 a9, uint64 p_currentLedge_mb, ParkourAction_Commonbase** p_newAction_out, float a12, float p_epsilon_mb));

namespace
{
    void SetGraphVariableInt(AtomGraph& atomGraph, uint32 rtcpIdx, int32 value)
    {
        if (!atomGraph.rtcp) return;
        uint32 offset = atomGraph.rtcp->graphVarsOffsets[rtcpIdx];
        *(int32*)(atomGraph.rtcp->graphVarsBuffer.arr + offset) = value;
    }
}

// ── Type-52 identification constants (runtime dump) ───────────────────────────

// The type-52 RUNTIME fancy vtable. Dump-verified: its fancy idx 0x45 slot
// (0x143A52BA0) holds 0x140199500, a two-instruction `mov eax, 0x34; ret`
// accessor returning EnumParkourAction 52 = wallrunUpFromGroundFailed_mb.
// This is the ONLY table in the binary whose 0x45 accessor returns 52.
// (The value previously used here, 0x143A6FFD0, returns 0x36 = 54 =
//  wallrunUpToHang -- see note 0) at the top of this file.)
static constexpr uint64 k_Type52FancyVTable_Runtime = 0x143A522F0;

// Transient base table installed by the shared base ctor (0x1401B9C80) before
// the post-init replaces it. Its fancy idx 0x45 is NULL -- calling
// GetEnumParkourAction() on an action carrying this table crashes.
// NOTE: its 0x4A slot holds 0x1401B5700, a quick-check SHARED by 22 tables, so
// this fallback can also match other action types. It is only used to avoid a
// null dispatch, never to identify the action for conversion.
static constexpr uint64 k_Type52FancyVTable_TransientBase = 0x143A41930;

// ── Type-52 per-type fit-test slots (the "wall context" gate) ────────────────
// Reached as fancy idx 0x4A from InitialTestIfActionFits (0x1401B4900). Both
// addresses below are read straight out of the dump; the vanilla values are
// re-verified at install time by the fail-safe further down.
static constexpr uintptr_t k_Type52FitTestSlot_Runtime  = 0x143A52C40;  // -> 0x14019B6F0
static constexpr uintptr_t k_Type52FitTestSlot_Transient = 0x143A42280; // -> 0x1401B5700 (shared)


// climbFacadeTester fancy vtables used by the optional cycle-start swap
// (ACUFixes-0.9.3 / ACU_ParkourPatches values; both valid tables in the dump
// and proven on this build by the working ACU_ParkourPatches.dll).
static constexpr uint64 k_ClimbFacadeTesterFancyVTable_Default = 0x1439E08D0;
static constexpr uint64 k_ClimbFacadeTesterFancyVTable_Baseclass = 0x1439FA1A0;

// ── Vanilla-byte fail-safe (BreakfallForce/ExactPositionTeleport pattern) ─────
// Before installing a code hook or swapping the fit-test slot, verify the site
// still holds the vanilla bytes/values captured in the 1.5.0 runtime dump. If
// anything else already patched a site (e.g. ACU_ParkourPatches.dll, which
// hooks 0x140135230), refuse to install and fail closed instead of stacking a
// second hook on foreign bytes. FailedWallRun and FreeJump share no patch
// sites, so within this DLL this only guards against third-party plugins and
// build mismatches.
static constexpr uint8_t k_ExpectedBytes_CreationSite[12] = {
    0xE9, 0xEB, 0xAE, 0x90, 0xFF, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }; // JMP 0x13FA40120 (Denuvo VM thunk) + game NOP padding
static constexpr uint8_t k_ExpectedBytes_RangeSetterSite[10] = {
    0xC7, 0x81, 0xF8, 0x01, 0x00, 0x00, 0xCD, 0xCC, 0x4C, 0x3D };             // MOV dword ptr [RCX+0x1F8], 0.05f
static constexpr uint8_t k_ExpectedBytes_WallingCycleSite[7] = {
    0xE9, 0x50, 0x29, 0xFD, 0xFD, 0x90, 0x90 };                               // JMP 0x13F871120 (Denuvo VM thunk)
static constexpr uint8_t k_ExpectedBytes_FailedWallrunCycleSite[7] = {
    0xE9, 0x0F, 0x47, 0xFF, 0xFD, 0x90, 0x90 };                               // JMP 0x13FA10120 (Denuvo VM thunk)

static bool SiteBytesMatch(uintptr_t addr, const uint8_t* expected, size_t len)
{
    const uint8_t* live = reinterpret_cast<const uint8_t*>(addr);
    return memcmp(live, expected, len) == 0;
}

// ── Null-safe action-type read ────────────────────────────────────────────────
// NEVER call AvailableParkourAction::GetEnumParkourAction() directly on actions
// that may be a wallrunUpFromGroundFailed_mb: some of its tables have a NULL
// fancy fn at idx 0x45 and GET_AND_CAST_FANCY_FUNC does no null check.
static int GetTypeIntSafe(const AvailableParkourAction* action)
{
    if (!action || !action->fancyVTable) return -1;
    const FancyVFunction& slot = action->fancyVTable->fancyVirtuals[ParkourActionKnownFancyVFuncs::GetEnumParkourAction::idx];
    if (!slot.fn)
    {
        // Null fancy-fn: identify the few known tables by pointer instead.
        if ((uint64)action->fancyVTable == k_Type52FancyVTable_Runtime
            || (uint64)action->fancyVTable == k_Type52FancyVTable_TransientBase)
            return (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
        return -1;
    }
    return (int)GET_AND_CAST_FANCY_FUNC(*action, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)((AvailableParkourAction*)action);
}

static bool IsType52Safe(const AvailableParkourAction* action)
{
    return GetTypeIntSafe(action) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
}

// ── Guard flag + cached settings for the static hooks ─────────────────────────
static volatile bool g_ShouldCreateFailedWallrun = false;
static volatile bool g_ExpandRanges = false;
static volatile bool g_SkipFitTest = false;
static volatile bool g_SwapTesterVTables = false;
static float g_LiftAmount = 2.0f;
static float g_LiftSpeed = 6.0f;
static int g_Action52Count = 0;
static int g_SabotagedCount = 0;
static int g_WidenCount = 0;
static int g_FitOverrideCount = 0;

// Permissive acceptance ranges so injected/natural type-52 survives validation.
// Applied with "only widen" semantics (never narrows vanilla) both inside the
// range-setter and after creation.
static float g_RangeHeightDiffMin = -3.0f;
static float g_RangeHeightDiffMax = 12.0f;  // vanilla type-52 max is 5.0 — stay a superset
static float g_RangeHorizDiffMin = -3.0f;
static float g_RangeHorizDiffMax = 3.0f;
static float g_RangeDistMin = 0.0f;
static float g_RangeDistMax = 15.0f;        // vanilla type-52 max is 12.0 — stay a superset
static float g_RangeCurveMax = 2.0f;
static float g_RangeCurveMin = -4.0f;
static float g_RangeExpectedCurveMin = -2.0f;

static bool IsTypeCompetingOnWalls(int t)
{
    return t == 53 || t == 65 || t == 73 || t == 32 || t == 44 || t == 77 || t == 70 || t == 35;
}
static bool IsTypeBeamSpecific(int t)
{
    return t == 10 || t == 12 || t == 60 || t == 100 || t == 101;
}

// Game function used to prune candidates (1.5.0 address, same value
// ForcedSidehopPlugin.cpp uses; do NOT DEFINE it here — it is already
// DEFINE_GAME_FUNCTION'd in GenericHooksInParkourFiltering.cpp).
using SmallArray_RemoveFunc = void(__fastcall*)(void* smallArray, int p_idx, unsigned int p_elemSize);
static SmallArray_RemoveFunc SmallArray_POD__RemoveGeneric = (SmallArray_RemoveFunc)0x142725F00;

uint64 FailedWallRunPlugin::s_Type52FancyVTable = 0;
static int g_CreationOverrideCount = 0;

// Raise an action's destination so Arno visibly gains height during the
// animation (Z is the vertical axis in AC Unity). Same adjustment the old
// plugin applied in OverrideCreationDiscard.
static void LiftActionDestination(AvailableParkourAction& action, float liftAmount)
{
    Vector3f& dest = (Vector3f&)action.locationAnchorDest;
    Vector3f& src = (Vector3f&)action.locationAnchorSrc;
    float riseTarget = src.z + liftAmount;
    if (dest.z < riseTarget)
    {
        dest.z = riseTarget;
        action.expectedVerticalSpeed_mb = g_LiftSpeed;
        action.expectedHeightDiff_mb = riseTarget - src.z;
        action.expectedVerticalDefaultDisplace = -1.0f;
    }
}

// ── Range widening (only-loosen semantics) ────────────────────────────────────
static void WidenMin(float& cur, float req) { if (req < cur) cur = req; }
static void WidenMax(float& cur, float req) { if (req > cur) cur = req; }

static void WidenType52Ranges(AvailableParkourAction& action)
{
    WidenMin(action.heightDifferenceMin, g_RangeHeightDiffMin);
    WidenMax(action.heightDifferenceMax, g_RangeHeightDiffMax);
    WidenMin(action.horizontalDifferenceMin, g_RangeHorizDiffMin);
    WidenMax(action.horizontalDifferenceMax, g_RangeHorizDiffMax);
    WidenMin(action.distanceMin, g_RangeDistMin);
    WidenMax(action.distanceMax, g_RangeDistMax);
    WidenMax(action.curveAllowedRangeMax, g_RangeCurveMax);
    WidenMin(action.curveAllowedRangeMin, g_RangeCurveMin);
    WidenMin(action.expectedCurveRangeMin, g_RangeExpectedCurveMin);
}

// ── Hook 2 callback: inside the type-52 range-setter ──────────────────────────
// Site 0x1401AF3B6 (10 bytes: `MOV dword ptr [RCX+0x1F8], 0x3D4CCCCD`) is on the
// convergence path of all ParkourMode branches of 0x1401AF2E0. RCX still holds
// the action there (it is never reassigned in that function). The stolen bytes
// re-execute after the callback, then control resumes at 0x1401AF3C0.
// 0x1401AF2E0 is referenced by exactly one fancy-table slot
// (0x143A70C60 = 0x143A6FFD0 + 0x64*0x20 + 0x10), so this only ever sees type-52
// actions.
static void WhenType52RangesSet_Widen(AllRegisters* params)
{
    if (!g_ExpandRanges) return;
    auto* action = (AvailableParkourAction*)params->rcx_;
    if (!action) return;
    WidenType52Ranges(*action);
    g_WidenCount++;
}

// ── Hook 3: fit-test override via fancy-table slot swap ─────────────────────
// The type-52 runtime fancy table (0x143A6FFD0) holds its per-type fit test at
// fancyVirtuals[0x4A].fn == 0x143A70920 (vanilla value 0x1401B0340, the
// geometry/quick-check gate). Dump-verified facts:
//   * 0x143A70920 is the ONLY qword in .data/.rdata holding 0x1401B0340, and
//     the table is installed only by the type-52 construction paths
//     (0x1401BC730 / 0x1401BC850) -> swapping this slot affects
//     wallrunUpFromGroundFailed_mb exclusively;
//   * the slot is static data: creation copies the table POINTER into the
//     object, it never rewrites the table's contents.
// While installed, the slot points at a forwarding stub that returns true
// (skip the wall-context gate) when g_SkipFitTest is set, else forwards to the
// vanilla test. This is the same "swap a game vtable entry per config" pattern
// the reverse-engineered ACU_ParkourPatches.dll uses.
// NOTE: a mid-function hook at the TEST/JZ after the per-type call was rejected
// deliberately: AutoAssemblerKinda copies stolen bytes verbatim (relative
// branches would break) and code hooks need at least 5 stolen bytes.
static constexpr uintptr_t k_Type52FitTestSlot_Address = 0x143A70920;
static void* const k_Type52FitTestOriginal = (void*)0x1401B0340;
static void* s_OriginalType52FitTestFn = nullptr;

static bool __fastcall Type52FitTestStub(
    AvailableParkourAction* action, void* a2, void* a3, void* a4,
    uint32 a5, uint64 a6, uint64 a7, uint64 a8)
{
    if (g_SkipFitTest)
    {
        g_FitOverrideCount++;
        return true;
    }
    using Fn = bool(__fastcall*)(AvailableParkourAction*, void*, void*, void*, uint32, uint64, uint64, uint64);
    return reinterpret_cast<Fn>(s_OriginalType52FitTestFn)(action, a2, a3, a4, a5, a6, a7, a8);
}

static bool SwapType52FitTestFn(bool install)
{
    void** slot = reinterpret_cast<void**>(k_Type52FitTestSlot_Address);
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    bool ok = false;
    if (install)
    {
        // Fail-safe rule: only swap when the slot still holds the vanilla fn.
        // If anything else already swapped it, refuse instead of stacking.
        if (*slot == k_Type52FitTestOriginal)
        {
            s_OriginalType52FitTestFn = k_Type52FitTestOriginal;
            *slot = (void*)&Type52FitTestStub;
            ok = true;
        }
    }
    else
    {
        // Restore only if the slot still holds OUR stub — never clobber a
        // patch someone else may have installed after us.
        if (*slot == (void*)&Type52FitTestStub)
        {
            *slot = s_OriginalType52FitTestFn ? s_OriginalType52FitTestFn : k_Type52FitTestOriginal;
            ok = true;
        }
    }
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    return ok;
}

// ── Hook 4 callbacks: cycle-start tester vtable swap (optional) ───────────────
static void SwapClimbFacadeTesterVTable(uint64 climbFacadeTester)
{
    if (!climbFacadeTester) return;
    *(uint64*)(climbFacadeTester + 8) = g_SwapTesterVTables
        ? k_ClimbFacadeTesterFancyVTable_Baseclass
        : k_ClimbFacadeTesterFancyVTable_Default;
}

static void WhenStartingWallingCycle_SwapFVTables(AllRegisters* params)
{
    if (!g_SwapTesterVTables) return;
    __try
    {
        WhileOnWallSystem* hsWalling = (WhileOnWallSystem*)params->rbx_;
        SwapClimbFacadeTesterVTable((uint64)hsWalling->climbFacade);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void WhenStartingFailedWallrunCycle_SwapFVTables(AllRegisters* params)
{
    if (!g_SwapTesterVTables) return;
    __try
    {
        uint64 hsFailedWallrun = params->rsi_;
        uint64 climbFacadeTester = *(uint64*)(hsFailedWallrun + 0x1D0);
        SwapClimbFacadeTesterVTable(climbFacadeTester);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Hook 1 callback: create the failed wallrun action during facade scans ─────
static void DuringClimbFacadeScan_CreateFailedWallrun(AllRegisters* params)
{
    if (!g_ShouldCreateFailedWallrun) return;

    // --- Count existing actions by type (null-safe!) ---
    auto& existingActions = *(SmallArray<ParkourAction_Commonbase*>*)params->rbx_;
    int action52Count = 0;
    int beamTypeCount = 0;
    for (int i = 0; i < existingActions.size; ++i)
    {
        if (!existingActions[i]) continue;
        int t = GetTypeIntSafe(existingActions[i]);
        if (t == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb) action52Count++;
        else if (IsTypeBeamSpecific(t)) beamTypeCount++;
    }

    // --- Create failed wallrun action ---
    ParkourAction_Commonbase* newAction = nullptr;
    bool isCreated = CreateParkourActionForParkourPointIfFits(
        EnumParkourAction::wallrunUpFromGroundFailed_mb,
        params->rdx_,
        (__m128*)params->rdi_,
        (__m128*)params->r9_,
        params->XMM6.f0,
        (int&)params->r13_,
        *(char*)(params->GetRSP() + 0x100),
        *(uint64*)(*(uint64*)params->r12_ + (int&)params->r15_),
        *(uint64*)(params->rbp_ + 0x118),
        *(uint64*)(params->rbp_ + 0x120),
        &newAction,
        *(float*)(params->rbp_ + 0x104),
        *(float*)(params->rbp_ + 0x108)
    );
    if (!isCreated || !newAction) return;

    action52Count++;

    // Expand allowed ranges (belt & braces — the widener hook already widened
    // them inside InitialTestIfActionFits when it was active).
    WidenType52Ranges(*newAction);

    // Boost fitness so it wins the sort
    newAction->fitness = 9999.0f;

    // Adjust destination height and expected speed for the lift animation
    {
        Vector3f& dest = (Vector3f&)newAction->locationAnchorDest;
        Vector3f& src = (Vector3f&)newAction->locationAnchorSrc;
        float topLedgeZ = newAction->locationTopLedge.z;
        float riseTarget = src.z + g_LiftAmount;
        if (topLedgeZ < 1e9f && topLedgeZ > src.z + 0.5f)
            riseTarget = std::min(topLedgeZ - 0.3f, src.z + g_LiftAmount + 1.0f);
        if (dest.z < riseTarget)
        {
            dest.z = riseTarget;
            newAction->expectedVerticalSpeed_mb = g_LiftSpeed;
            newAction->expectedHeightDiff_mb = riseTarget - src.z;
            newAction->expectedVerticalDefaultDisplace = -1.0f;
        }
    }

    // --- Sabotage competing actions in the existing array ---
    int sabotagedCount = 0;
    for (int i = 0; i < existingActions.size; ++i)
    {
        AvailableParkourAction* action = existingActions[i];
        if (!action) continue;
        int t = GetTypeIntSafe(action);

        // On walls: sabotage wall competitors so type 52 wins
        if (IsTypeCompetingOnWalls(t))
        {
            action->fitness = 0.0f;
            sabotagedCount++;
        }
    }

    // Append the new action
    ACU::Memory::SmallArrayAppend(existingActions, newAction);

    g_Action52Count = action52Count;
    g_SabotagedCount = sabotagedCount;
}

// ── Hook constructors ─────────────────────────────────────────────────────────

GPH_FailedWallrunCreator::GPH_FailedWallrunCreator()
{
    PresetScript_CCodeInTheMiddle(0x140135230, 12,
        DuringClimbFacadeScan_CreateFailedWallrun,
        AutoAssemblerCodeHolder_Base::RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
}

GPH_Type52RangeWidener::GPH_Type52RangeWidener()
{
    PresetScript_CCodeInTheMiddle(0x1401AF3B6, 10,
        WhenType52RangesSet_Widen,
        AutoAssemblerCodeHolder_Base::RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
}

GPH_CycleStartVTableSwap::GPH_CycleStartVTableSwap()
{
    PresetScript_CCodeInTheMiddle(0x141A4D7CB, 7,
        WhenStartingWallingCycle_SwapFVTables,
        AutoAssemblerCodeHolder_Base::RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    PresetScript_CCodeInTheMiddle(0x141A1BA0C, 7,
        WhenStartingFailedWallrunCycle_SwapFVTables,
        AutoAssemblerCodeHolder_Base::RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
}

// ── Settings persistence ──────────────────────────────────────────────────────

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\FailedWallRunPlugin.ini";
    return "FailedWallRunPlugin.ini";
}

void FailedWallRunPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        // Defensive: a malformed or unexpectedly short line must never throw
        // out of the init stage (an uncaught std::out_of_range here previously
        // terminated the process at game startup through the loader's plugin
        // init call — the substr offsets below must match their key lengths).
        try
        {
            if (line.rfind("ToggleKey=", 0) == 0)                 // 10 chars
                try { m_ToggleKey = std::stoi(line.substr(10), nullptr, 16); } catch (...) {}
            else if (line.rfind("Enabled=", 0) == 0)              // 8 chars
                m_PluginEnabled = (line.substr(8) == "1");
            else if (line.rfind("HoldMode=", 0) == 0)             // 9 chars
                try { m_HoldMode = std::stoi(line.substr(9)) != 0; } catch (...) {}
            else if (line.rfind("ToggleState=", 0) == 0)          // 12 chars
                try { m_ToggleState = std::stoi(line.substr(12)) != 0; } catch (...) {}
            else if (line.rfind("LiftAmount=", 0) == 0)           // 11 chars
                try { m_LiftAmount = std::stof(line.substr(11)); } catch (...) {}
            else if (line.rfind("AnimationCancel=", 0) == 0)      // 16 chars
                m_CancelEnabled = (line.substr(16) == "1");
            else if (line.rfind("LiftSpeed=", 0) == 0)            // 10 chars
                try { m_LiftSpeed = std::stof(line.substr(10)); } catch (...) {}
            else if (line.rfind("EnforceEnabled=", 0) == 0)       // 15 chars
                m_EnforceEnabled = (line.substr(15) == "1");
            else if (line.rfind("RangeWidenerEnabled=", 0) == 0)  // 20 chars
                m_RangeWidenerEnabled = (line.substr(20) == "1");
            else if (line.rfind("SkipFitTest=", 0) == 0)          // 12 chars
                m_SkipFitTestEnabled = (line.substr(12) == "1");
            else if (line.rfind("VtableSwapEnabled=", 0) == 0)    // 18 chars
                m_VtableSwapEnabled = (line.substr(18) == "1");
            else if (line.rfind("SingleStageMode=", 0) == 0)      // 16 chars
                m_SingleStageMode = (line.substr(16) == "1");
        }
        catch (...)
        {
            // Never let INI parsing take the game down.
        }
    }
}

void FailedWallRunPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetIniPath());
        if (file)
        {
            file << "ToggleKey=" << std::hex << m_ToggleKey << std::dec << "\n";
            file << "Enabled=" << (m_PluginEnabled ? 1 : 0) << "\n";
            file << "HoldMode=" << (m_HoldMode ? 1 : 0) << "\n";
            file << "ToggleState=" << (m_ToggleState ? 1 : 0) << "\n";
            file << "LiftAmount=" << m_LiftAmount << "\n";
            file << "AnimationCancel=" << (m_CancelEnabled ? 1 : 0) << "\n";
            file << "LiftSpeed=" << m_LiftSpeed << "\n";
            file << "EnforceEnabled=" << (m_EnforceEnabled ? 1 : 0) << "\n";
            file << "RangeWidenerEnabled=" << (m_RangeWidenerEnabled ? 1 : 0) << "\n";
            file << "SkipFitTest=" << (m_SkipFitTestEnabled ? 1 : 0) << "\n";
            file << "VtableSwapEnabled=" << (m_VtableSwapEnabled ? 1 : 0) << "\n";
            file << "SingleStageMode=" << (m_SingleStageMode ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

bool FailedWallRunPlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}

bool FailedWallRunPlugin::IsFeatureActive() const
{
    if (!m_PluginEnabled) return false;
    if (m_HoldMode)
        return (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    else
        return m_ToggleState;
}

FailedWallRunPlugin::FailedWallRunPlugin()
{
    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction*
    {
        return static_cast<FailedWallRunPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.ChooseAfterSorting_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame) -> AvailableParkourAction*
    {
        return static_cast<FailedWallRunPlugin*>(userData)->ChooseAfterSorting(actions, selectedByGame);
    };
    m_Callbacks.OverrideCreationDiscard_fnp = [](void* userData, AvailableParkourAction& action) -> bool
    {
        return static_cast<FailedWallRunPlugin*>(userData)->OverrideCreationDiscard(action);
    };
    m_Callbacks.m_UserData = this;
    // Distinct priority (proven pattern: ForcedSidehop used 4.0 vs FreeJump's
    // 5.0). Equal priorities would make callback order depend on subscription
    // order, i.e. on which key was pressed first after load. 4.5 =
    // deterministic: FailedWallRun's force-select runs before FreeJump's array
    // wipe/overrides when both keys are held (the GPH dispatcher short-circuits
    // after a callback returns an action, so FreeJump's pass is skipped for
    // that cycle — no interference in either direction).
    m_Callbacks.m_CallbackPriority = 4.5f;
    m_Callbacks.m_Name = "FailedWallRunPlugin";
}

FailedWallRunPlugin::~FailedWallRunPlugin()
{
    if (m_GphActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActivatorCreation.reset();
        m_GphActive = false;
    }
    if (m_CreatorHookActive) { m_FailedWallrunCreator.Deactivate(); m_CreatorHookActive = false; }
    if (m_WidenerHookActive) { m_RangeWidener.Deactivate(); m_WidenerHookActive = false; }
    if (m_FitTestHookActive) { SwapType52FitTestFn(false); m_FitTestHookActive = false; }
    if (m_VTableSwapHookActive) { m_VTableSwapHook.Deactivate(); m_VTableSwapHookActive = false; }
}

// ── Sort&select enforcement ───────────────────────────────────────────────────

AvailableParkourAction* FailedWallRunPlugin::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    // Single-stage mode (ForcedSidehop-style): skip enforcement callbacks.
    if (m_SingleStageMode) return nullptr;

    if (!IsFeatureActive() || !m_EnforceEnabled) return nullptr;

    // First pass: count type 52 in the array (null-safe reads only).
    int action52Count = 0;
    for (int i = 0; i < (int)actions.size; ++i)
    {
        if (!actions[i]) continue;
        if (GetTypeIntSafe(actions[i]) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb)
            action52Count++;
    }

    // Second pass: manipulate fitness to force selection of type 52.
    // On walls: type 52 was kept alive by the creation override, so sabotage
    //   wall competitors.
    // On beams: no type 52 exists, so sabotage beam types and boost type 53
    //   (which OverrideCreationDiscard converted to 52 via vtable swap — after
    //   conversion it reads as 52 in the first pass).
    int sabotagedCount = 0;
    for (int i = 0; i < (int)actions.size; ++i)
    {
        AvailableParkourAction* action = actions[i];
        if (!action) continue;
        const int t = GetTypeIntSafe(action);

        if (t == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb)
        {
            action->fitness = 9999.0f;
        }
        else if (t == (int)EnumParkourAction::wallrunUpToHang) // 54
        {
            SmallArray_POD__RemoveGeneric(&actions, i--, 8u);
            sabotagedCount++;
        }
        else if (action52Count > 0 && IsTypeCompetingOnWalls(t))
        {
            action->fitness = 0.0f;
            sabotagedCount++;
        }
        else if (action52Count == 0)
        {
            if (IsTypeBeamSpecific(t))
            {
                action->fitness = 0.0f;
                sabotagedCount++;
            }
            else if (t == (int)EnumParkourAction::wallrunUpFromGround_mb) // 53
            {
                action->fitness = 9999.0f;
            }
        }
    }

    m_LastAction52Count = action52Count;
    m_LastSabotagedCount = sabotagedCount;

    // Force-select the first failed wallrun if one is in the array
    // (converted 53s read as 52 here because the vtable was swapped).
    for (int i = 0; i < (int)actions.size; ++i)
    {
        AvailableParkourAction* action = actions[i];
        if (!action) continue;
        if (IsType52Safe(action))
            return action;
    }
    return nullptr;
}

AvailableParkourAction* FailedWallRunPlugin::ChooseAfterSorting(SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame)
{
    if (m_SingleStageMode) return selectedByGame;

    if (!IsFeatureActive() || !m_EnforceEnabled) return selectedByGame;
    for (int i = 0; i < (int)actions.size; ++i)
    {
        AvailableParkourAction* action = actions[i];
        if (!action) continue;
        // 52 = failed wallrun; 53 = normal wallrun-up kept as a defensive
        // fallback (post-conversion actions read as 52 anyway).
        const int t = GetTypeIntSafe(action);
        if (t == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb
            || t == (int)EnumParkourAction::wallrunUpFromGround_mb)
            return action;
    }
    return nullptr;
}

// ── Creation-stage override (requires GPH creation replacements) ─────────────

bool FailedWallRunPlugin::OverrideCreationDiscard(AvailableParkourAction& action)
{
    // Single-stage mode = ForcedSidehop-style: no creation-stage mutation.
    if (m_SingleStageMode) return false;
    if (!IsFeatureActive() || !m_EnforceEnabled) return false;

    // Null-safe type read (fancy idx 0x45 can be NULL on transient tables).
    const int t = GetTypeIntSafe(&action);

    // Capture type 52's RUNTIME fancy vtable once — only from the
    // dump-verified runtime table, never from the transient base table.
    if (t == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb
        && s_Type52FancyVTable == 0
        && (uint64)action.fancyVTable == k_Type52FancyVTable_Runtime)
    {
        s_Type52FancyVTable = (uint64)action.fancyVTable;
    }

    // Keep the failed wallrun alive even when the fit test rejected it.
    if (t == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb)
    {
        LiftActionDestination(action, m_LiftAmount);
        g_CreationOverrideCount++;
        return true;
    }

    // Convert the normal wallrun-up (53) into the failed wallrun (52) by
    // swapping its fancy vtable — the same behavior-swap pattern
    // ForcedSidehopPlugin uses. Requires a captured type-52 vtable.
    if (t == (int)EnumParkourAction::wallrunUpFromGround_mb && s_Type52FancyVTable != 0)
    {
        action.fancyVTable = reinterpret_cast<ParkourAction_FancyVTable*>(s_Type52FancyVTable);
        LiftActionDestination(action, m_LiftAmount + 0.5f);
        g_CreationOverrideCount++;
        return true;
    }

    return false;
}

// ── Per-frame update ──────────────────────────────────────────────────────────

void FailedWallRunPlugin::OnUpdate()
{
    // Handle key rebinding
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (IsRisingEdgePressed(vk))
            {
                m_ToggleKey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
        return;
    }

    // Update toggle state if in toggle mode
    if (m_PluginEnabled && !m_HoldMode)
    {
        if (IsRisingEdgePressed(m_ToggleKey))
        {
            m_ToggleState = !m_ToggleState;
            SaveSettings();
        }
    }

    // -- Single-stage mode (exact ForcedSidehop-style): disable all extra
    // hooks (creation, widener, fit-override, cycle-swap) and only use GPH.
    bool singleStage = m_SingleStageMode;
    bool shouldBeActive = IsFeatureActive() && m_EnforceEnabled;
    if (singleStage)
    {
        // Force only GPH-based behavior; disable all injection layers.
        g_ShouldCreateFailedWallrun = false;
        g_ExpandRanges = false;
        g_SkipFitTest = false;
        g_SwapTesterVTables = false;
    }
    else
    {
        g_ShouldCreateFailedWallrun = shouldBeActive;
        g_ExpandRanges = shouldBeActive && m_RangeWidenerEnabled;
        g_SkipFitTest = shouldBeActive && m_SkipFitTestEnabled;
        g_SwapTesterVTables = shouldBeActive && m_VtableSwapEnabled;
    }
    g_LiftAmount = m_LiftAmount;
    g_LiftSpeed = m_LiftSpeed;

    // Deactivate any extra-layer hooks when in single-stage mode.
    // Every activation is gated on the site still holding vanilla bytes
    // (fail closed on foreign patches — BreakfallForce/ExactPositionTeleport
    // fail-safe pattern). Gated hooks that fail verification simply stay
    // inactive and retry each frame; the UI shows the per-site check state.
    if (singleStage && m_CreatorHookActive)
    {
        m_FailedWallrunCreator.Deactivate();
        m_CreatorHookActive = false;
    } else if (!m_SingleStageMode) {
        if (shouldBeActive && !m_CreatorHookActive)
        {
            m_CreatorSiteOk = SiteBytesMatch(0x140135230, k_ExpectedBytes_CreationSite, sizeof(k_ExpectedBytes_CreationSite));
            if (m_CreatorSiteOk)
            {
                m_FailedWallrunCreator.Activate();
                m_CreatorHookActive = true;
            }
        }
        else if (!shouldBeActive && m_CreatorHookActive)
        {
            m_FailedWallrunCreator.Deactivate();
            m_CreatorHookActive = false;
        }
    }

    if (singleStage && m_WidenerHookActive)
    {
        m_RangeWidener.Deactivate();
        m_WidenerHookActive = false;
    } else if (!m_SingleStageMode) {
        if (shouldBeActive && m_RangeWidenerEnabled && !m_WidenerHookActive)
        {
            m_WidenerSiteOk = SiteBytesMatch(0x1401AF3B6, k_ExpectedBytes_RangeSetterSite, sizeof(k_ExpectedBytes_RangeSetterSite));
            if (m_WidenerSiteOk)
            {
                m_RangeWidener.Activate();
                m_WidenerHookActive = true;
            }
        }
        else if ((!shouldBeActive || !m_RangeWidenerEnabled) && m_WidenerHookActive)
        {
            m_RangeWidener.Deactivate();
            m_WidenerHookActive = false;
        }
    }

    if (singleStage && m_FitTestHookActive)
    {
        SwapType52FitTestFn(false);
        m_FitTestHookActive = false;
    } else if (!m_SingleStageMode) {
        if (shouldBeActive && m_SkipFitTestEnabled && !m_FitTestHookActive)
        {
            m_FitTestSlotOk = SwapType52FitTestFn(true);
            m_FitTestHookActive = m_FitTestSlotOk;
        }
        else if ((!shouldBeActive || !m_SkipFitTestEnabled) && m_FitTestHookActive)
        {
            SwapType52FitTestFn(false);
            m_FitTestHookActive = false;
        }
    }

    if (singleStage && m_VTableSwapHookActive)
    {
        m_VTableSwapHook.Deactivate();
        m_VTableSwapHookActive = false;
    } else if (!m_SingleStageMode) {
        if (shouldBeActive && m_VtableSwapEnabled && !m_VTableSwapHookActive)
        {
            m_SwapSitesOk = SiteBytesMatch(0x141A4D7CB, k_ExpectedBytes_WallingCycleSite, sizeof(k_ExpectedBytes_WallingCycleSite))
                         && SiteBytesMatch(0x141A1BA0C, k_ExpectedBytes_FailedWallrunCycleSite, sizeof(k_ExpectedBytes_FailedWallrunCycleSite));
            if (m_SwapSitesOk)
            {
                m_VTableSwapHook.Activate();
                m_VTableSwapHookActive = true;
            }
        }
        else if ((!shouldBeActive || !m_VtableSwapEnabled) && m_VTableSwapHookActive)
        {
            m_VTableSwapHook.Deactivate();
            m_VTableSwapHookActive = false;
        }
    }

    // Sort&select + creation-stage enforcement via GenericHooksInParkourFiltering
    // In single-stage mode we disable the GPH callbacks that enforce type-52
    // and instead rely ONLY on the ForcedSidehop-style rewrite approach.
    bool useGphEnforce = !m_SingleStageMode;
    if (useGphEnforce && shouldBeActive && !m_GphActive)
    {
        auto gph = GenericHooksInParkourFiltering::GetSingleton();
        gph->Subscribe(m_Callbacks);
        m_HookActivator = gph->RequestGPHSortAndSelect();
        // Creation-stage replacements (0x1401D1CF0 / 0x1401D1E50) are what
        // invoke OverrideCreationDiscard_fnp — required for the keep-alive and
        // 53->52 conversion. Same proven pattern the old plugin used.
        m_HookActivatorCreation = gph->RequestGPHCreation();
        m_GphActive = true;
    }
    else if (!m_SingleStageMode && !shouldBeActive && m_GphActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActivatorCreation.reset();
        m_GphActive = false;
    }

    // Refresh debug stats from the globals
    m_LastAction52Count = g_Action52Count;
    m_LastSabotagedCount = g_SabotagedCount;

    // --- Ezio-style Animation Cancel ---
    if (m_CancelEnabled)
    {
        if (IsRisingEdgePressed(VK_SPACE) && IsInParkourState())
        {
            HumanStatesHolder* holder = HumanStatesHolder::GetForPlayer();
            if (holder && holder->atomAnimCpnt && holder->atomAnimCpnt->shared_AtomGraph_NewDemo_DEV)
            {
                AtomGraph* graph = holder->atomAnimCpnt->shared_AtomGraph_NewDemo_DEV->GetPtr();
                if (graph && graph->rtcp)
                {
                    SetGraphVariableInt(*graph, kGeneralStateRTCPIdx, 0);
                    SetGraphVariableInt(*graph, kParkourRTCPIdx, 0);
                }
            }
        }
    }
}

bool FailedWallRunPlugin::IsInParkourState() const
{
    HumanStatesHolder* holder = HumanStatesHolder::GetForPlayer();
    if (!holder || !holder->atomAnimCpnt || !holder->atomAnimCpnt->shared_AtomGraph_NewDemo_DEV) return false;

    AtomGraph* graph = holder->atomAnimCpnt->shared_AtomGraph_NewDemo_DEV->GetPtr();
    if (!graph || !graph->rtcp) return false;

    uint32 offset = graph->rtcp->graphVarsOffsets[kGeneralStateRTCPIdx];
    int32 generalState = *(int32*)(graph->rtcp->graphVarsBuffer.arr + offset);
    return generalState == 5;
}

void FailedWallRunPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
    const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);
    const ImVec4 debugColor(0.60f, 0.60f, 1.00f, 1.00f);

    ImGui::Text("Failed Wall-Run Plugin");
    ImGui::Separator();

    ImGui::TextWrapped("Forces the failed wallrun (wallrunUpFromGroundFailed_mb) on ANY wall, "
                       "with or without handholds. Ranges are widened during creation and the "
                       "wall-context fit test is overridden for type 52 while the key is held.");
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Failed Wallrun Side-Eject", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool enabled = m_PluginEnabled;
        if (ImGui::Checkbox("Enable Failed Wallrun", &enabled))
        {
            m_PluginEnabled = enabled;
            SaveSettings();
        }

        if (m_PluginEnabled)
        {
            ImGui::Indent();

            bool isActive = IsFeatureActive();
            ImGui::Text("Status:"); ImGui::SameLine();
            ImGui::TextColored(isActive ? activeColor : inactiveColor,
                               isActive ? "ACTIVE" : "INACTIVE");

            bool holdMode = m_HoldMode;
            if (ImGui::RadioButton("Hold (key held)", holdMode))
            {
                m_HoldMode = true;
                SaveSettings();
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Toggle (press to toggle)", !holdMode))
            {
                m_HoldMode = false;
                SaveSettings();
            }

            ImGui::Text("Trigger key: 0x%02X", m_ToggleKey);
            if (m_WaitingForKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Press any key..."); }
            if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind##fwr"))
                m_WaitingForKey = !m_WaitingForKey;

            ImGui::Separator();
            ImGui::TextColored(debugColor, "Patches (all 1.5.0 dump-verified):");

            bool enforce = m_EnforceEnabled;
            if (ImGui::Checkbox("Enforce failed wallrun", &enforce))
            {
                m_EnforceEnabled = enforce;
                SaveSettings();
            }

            if (m_EnforceEnabled)
            {
                ImGui::Indent();

                bool widener = m_RangeWidenerEnabled;
                if (ImGui::Checkbox("Widen acceptance ranges (during creation)", &widener))
                {
                    m_RangeWidenerEnabled = widener;
                    SaveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hooks the type-52 SetExpectedDistanceRangesAccordingToParkourMode\n"
                                      "(0x1401AF2E0) so the widened ranges are in place BEFORE the\n"
                                      "fit test runs. Only ever loosens vanilla ranges.");

                bool skipFit = m_SkipFitTestEnabled;
                if (ImGui::Checkbox("Override wall-context fit test", &skipFit))
                {
                    m_SkipFitTestEnabled = skipFit;
                    SaveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Forces the per-type fit-test verdict (fancy 0x4A ->\n"
                                      "0x1401B5700 quick-check chain) to 'fits' for type-52 actions\n"
                                      "only. This is what makes the move work on walls WITH handholds.");

                bool singleStageChk = m_SingleStageMode;
                if (ImGui::Checkbox("Single-stage mode (ForcedSidehop-style)", &singleStageChk))
                {
                    m_SingleStageMode = singleStageChk;
                    SaveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("When enabled: disables all injection hooks (creation, widener, fit-override, cycle-swap) and uses only GPH like ForcedSidehopPlugin.");

                bool vtableSwap = m_VtableSwapEnabled;
                if (ImGui::Checkbox("Cycle-start tester vtable swap (experimental)", &vtableSwap))
                {
                    m_VtableSwapEnabled = vtableSwap;
                    SaveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ACUFixes/ACU_ParkourPatches pattern: swap the climbFacadeTester\n"
                                      "fancy vtable at walling/failed-wallrun cycle start while the key\n"
                                      "is held, so more move types get scanned on walls.");

                ImGui::Text("Lift height:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                float liftH = m_LiftAmount;
                if (ImGui::InputFloat("m##liftheight", &liftH, 0.1f, 0.5f, "%.1f"))
                {
                    if (liftH < 0.0f) liftH = 0.0f;
                    if (liftH > 10.0f) liftH = 10.0f;
                    m_LiftAmount = liftH;
                    SaveSettings();
                }

                ImGui::Text("Lift speed:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                float liftS = m_LiftSpeed;
                if (ImGui::InputFloat("m/s##liftspeed", &liftS, 0.5f, 1.0f, "%.1f"))
                {
                    if (liftS < 0.5f) liftS = 0.5f;
                    if (liftS > 20.0f) liftS = 20.0f;
                    m_LiftSpeed = liftS;
                    SaveSettings();
                }

                ImGui::Unindent();
            }

            if (isActive)
            {
                ImGui::Separator();
                ImGui::TextColored(debugColor, "Stats (last scan cycle):");
                ImGui::Text("  Type-52 in list: %d | Sabotaged competitors: %d",
                    m_LastAction52Count,
                    m_LastSabotagedCount);
                ImGui::Text("  Range widenings: %d | Fit-test overrides: %d",
                    g_WidenCount, g_FitOverrideCount);
                ImGui::Text("  Kept/converted at creation: %d",
                    g_CreationOverrideCount);
                if (s_Type52FancyVTable == 0)
                    ImGui::TextColored(ImVec4(1, 1, 0, 1),
                        "  (capturing type-52 vtable... approach any wall once)");
                ImGui::Text("  Hooks: creation %s | widener %s | fit-override %s | vtable-swap %s",
                    m_CreatorHookActive ? "ON" : "off",
                    m_WidenerHookActive ? "ON" : "off",
                    m_FitTestHookActive ? "ON" : "off",
                    m_VTableSwapHookActive ? "ON" : "off");
                bool sitesOk = m_CreatorSiteOk && m_WidenerSiteOk && m_FitTestSlotOk && m_SwapSitesOk;
                ImGui::Text("  Site checks (vanilla bytes): %s",
                    sitesOk ? "OK" : "MISMATCH — a hook site was modified by something else;"
                                      " affected layer refused to install");
                if (!sitesOk)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f),
                        "    creation:%s widener:%s fit-slot:%s swap:%s",
                        m_CreatorSiteOk ? "ok" : "MODIFIED",
                        m_WidenerSiteOk ? "ok" : "MODIFIED",
                        m_FitTestSlotOk ? "ok" : "MODIFIED",
                        m_SwapSitesOk ? "ok" : "MODIFIED");
                }
            }

            ImGui::Unindent();
        }
    }

    // --- Section 2: Animation Cancel ---
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Animation Cancel (Ezio-Style)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped(
            "Instantly cancels any parkour animation mid-motion when you press Space, "
            "letting you chain into a new action immediately.");
        ImGui::Spacing();

        bool cancelEnabled = m_CancelEnabled;
        if (ImGui::Checkbox("Enable Animation Cancel", &cancelEnabled))
        {
            m_CancelEnabled = cancelEnabled;
            SaveSettings();
        }

        if (m_CancelEnabled)
        {
            ImGui::Indent();
            ImGui::Separator();
            ImGui::TextColored(debugColor, "Status:");
            ImGui::Text("  In Parkour State: %s", IsInParkourState() ? "YES" : "NO");
            ImGui::Text("  Cancel Key: Space");
            ImGui::TextColored(m_SingleStageMode ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                             "  Mode: %s", m_SingleStageMode ? "SINGLE-STAGE (ForcedSidehop-style)" : "FULL (creation + injection)");
            ImGui::Unindent();
        }
    }
}
