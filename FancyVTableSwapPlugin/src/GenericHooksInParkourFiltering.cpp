#include "pch.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include <atomic>
#include <cstdio>
#include "ACU/basic_types.h"
#include "ACU/Memory/ACUAllocs.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ParkourDebugging/ParkourTester.h"
#include "ParkourDebugging/ParkourLog.h"
#include "ParkourDebugging/GetTypeIntSafe.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ACU_DefineNativeFunction.h"
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdint>

// AvailableParkourAction::GetEnumParkourAction() - single definition for this
// DLL. ForcedSidehopPlugin.cpp calls it but must not redefine it.
EnumParkourAction AvailableParkourAction::GetEnumParkourAction()
{
    return GET_AND_CAST_FANCY_FUNC(*this, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)(this);
}

// All game addresses below are the ACUFixes-0.9.3 values, i.e. the set that
// matches the user's ACU 1.5.0 Steam build.
//
// Evidence (2026-10-28): a constant scan of the two plugins that demonstrably
// work on this install (ACUFixes.dll and ACU_ParkourPatches.dll in
// ACUFixes\plugins) finds exactly this address set in both, and finds NONE of
// the ACUFixes-1.5.1 addresses (0x1401D4360 / 0x1401D2580 / 0x1401CBF00 /
// 0x1401D1530 / 0x140159940 / 0x142726000 / 0x14013493E / 0x140134940).
// FailedWallRunPlugin.cpp additionally dump-verifies 0x1401D1CF0 and
// 0x1401CC990 against the runtime dump (MODLOG/scripts/ACU_dumped_pe.exe).
//
// The previous version of this file used the 1.5.1 addresses and installed a
// 7-stolen-byte mid-function hook at 0x14013493E, which lands on unrelated
// code in the 1.5.0 binary: any parkour sort&select execution after enabling
// the plugin instantly crashed the game (see PROGRESS.md).
DEFINE_GAME_FUNCTION(AvailableParkourAction__FinalFilter1, 0x1401D4DE0, bool, __fastcall, (AvailableParkourAction*, __m128*, uint64, Entity*));
DEFINE_GAME_FUNCTION(AvailableParkourAction__FinalFilter2, 0x1401D3000, bool, __fastcall, (AvailableParkourAction*, Entity*, __int64));
DEFINE_GAME_FUNCTION(ConstructParkourAction_A, 0x1401CC990, AvailableParkourAction*, __fastcall, (EnumParkourAction, __m128*, __m128*, __m128*, int, char, __int64, AvailableParkourAction*));
DEFINE_GAME_FUNCTION(ConstructParkourAction_B, 0x1401D1FC0, AvailableParkourAction*, __fastcall, (EnumParkourAction, __m128*, __m128*, __m128*, int, char, __int64, __int64, Entity*, AvailableParkourAction*));
DEFINE_GAME_FUNCTION(AvailableParkourAction__InitializePlayerRef, 0x14015A570, void, __fastcall, (PlayerRefInParkourAction*, Entity*));
DEFINE_GAME_FUNCTION(SmallArray_POD__RemoveGeneric, 0x142725F00, void, __fastcall, (void*, int, unsigned int));

// Defined in FancyVTableSwapPlugin.cpp — true while the failed-wallrun
// enforcer is armed and the action is type 52 (skip the ~zero-fitness cull).
bool FVTS_ShouldSkipFitnessCull(AvailableParkourAction* action);

class Entity;

// ── Creation-stage replacements (ported verbatim from ACUFixes-0.9.3) ──────
// Replace the two creation wrappers at their function starts (0x1401D1CF0 /
// 0x1401D1E50, the ACUFixes-0.9.3 address set, confirmed present in the two
// reference DLLs that work on this install — see PROGRESS.md session 6).
// Difference from the 0.9.3 original: after InitialTestIfActionFits, every
// subscribed callback gets a chance to override the discard verdict via
// OverrideCreationDiscard_fnp (return true = keep the action; the callback may
// have mutated it in place first — FailedWallRunPlugin keeps
// wallrunUpFromGroundFailed_mb alive on non-qualifying walls and converts
// wallrunUpFromGround_mb into it via a fancy-vtable swap).
struct CollisionProbeForParkour_mb
{
    Vector4f locationAnchorDest; //0x0000
    Vector4f direction; //0x0010
    char pad_0020[20]; //0x0020
    float flt_34; //0x0034
    uint32 collisionLayer_mb; //0x0038
    char pad_003C[4]; //0x003C
    Vector4f pos40; //0x0040
    Vector4f pos50; //0x0050
    PlayerRefInParkourAction shared_buildingEntity; //0x0060
    char pad_0078[8]; //0x0078
}; //Size: 0x0080
assert_offsetof(CollisionProbeForParkour_mb, collisionLayer_mb, 0x38);
assert_offsetof(CollisionProbeForParkour_mb, shared_buildingEntity, 0x60);
assert_sizeof(CollisionProbeForParkour_mb, 0x80); // Allocated and constructed at 140184AEF

static void RunCreationDiscardOverrides(AvailableParkourAction& newAction, bool& isDiscarded)
{
    auto& allParkourCallbacks = GenericHooksInParkourFiltering::GetSingleton()->m_Callbacks;
    for (ParkourCallbacks* parkourCallbacks : allParkourCallbacks)
    {
        if (!parkourCallbacks->OverrideCreationDiscard_fnp) continue;
        if (parkourCallbacks->OverrideCreationDiscard_fnp(parkourCallbacks->m_UserData, newAction))
            isDiscarded = false;
    }
}

static void RunPostConstruct(AvailableParkourAction& newAction)
{
    auto& allParkourCallbacks = GenericHooksInParkourFiltering::GetSingleton()->m_Callbacks;
    for (ParkourCallbacks* parkourCallbacks : allParkourCallbacks)
    {
        if (!parkourCallbacks->PostConstruct_fnp) continue;
        parkourCallbacks->PostConstruct_fnp(parkourCallbacks->m_UserData, newAction);
    }
}

bool CreateParkourActionAndPerformInitialTestIfFits_A_FullReplacement(
    EnumParkourAction actionType,
    __m128* p_locationOfOrigin,
    __m128* a3,
    __m128* p_directionOfMovementInputWorldSpace,
    float a5, int a6, char a7, CollisionProbeForParkour_mb* a8,
    Entity* p_player,
    AvailableParkourAction* p_previousAction_mb,
    AvailableParkourAction** const p_newAction_out,
    float a12,
    float p_epsilon_mb)
{
    AvailableParkourAction* newAction = ConstructParkourAction_A(actionType, p_locationOfOrigin, a3, p_directionOfMovementInputWorldSpace, a6, a7, (uint64)a8, p_previousAction_mb);
    *p_newAction_out = newAction;
    if (!newAction) return false;
    // ENFORCE-52: swap the table BEFORE any type-specific creation call, so
    // Set2Floats/InitialTest/fitness all run with the final table. Converting
    // only afterwards crashed (type-52 fns walked type-53-initialized data).
    RunPostConstruct(*newAction);
    if (p_player)
        AvailableParkourAction__InitializePlayerRef(&newAction->playerRef, p_player);
    GET_AND_CAST_FANCY_FUNC(*newAction, ParkourActionKnownFancyVFuncs::Set2FloatsAfterCreation)(newAction, a12, p_epsilon_mb);
    bool isActionFits = GET_AND_CAST_FANCY_FUNC(*newAction, ParkourActionKnownFancyVFuncs::InitialTestIfActionFits)(
        newAction,
        p_locationOfOrigin,
        a3,
        p_directionOfMovementInputWorldSpace,
        a5,
        a6,
        (uint64)a8,
        p_player,
        p_previousAction_mb);
    bool isDiscarded = !isActionFits;
    RunCreationDiscardOverrides(*newAction, isDiscarded);
    std::shared_ptr<ParkourCycleLogged> currentCycle = GetCurrentLoggedParkourCycle();
    currentCycle->LogActionInitialCreation(*newAction, isDiscarded);
    currentCycle->m_LocationOfOrigin = *(Vector3f*)p_locationOfOrigin;
    currentCycle->m_DirectionOfMovementInputWorldSpace = *(Vector3f*)p_directionOfMovementInputWorldSpace;
    if (isDiscarded)
    {
        newAction->Unk008_Destroy(0);
        ACU::Memory::ACUDeallocateBytes((byte*)newAction);
        *p_newAction_out = nullptr;
        return false;
    }
    return true;
}

bool CreateParkourActionAndPerformInitialTestIfFits_B_FullReplacement(
    EnumParkourAction actionType,
    __m128* p_locationOfOrigin,
    __m128* a3,
    __m128* p_directionOfMovementInputWorldSpace,
    float a5, int a6, char a7, uint64 a8, uint64 a9,
    Entity* p_player,
    AvailableParkourAction* p_previousAction_mb,
    AvailableParkourAction** const p_newAction_out,
    float a13,
    float p_epsilon_mb)
{
    AvailableParkourAction* newAction = ConstructParkourAction_B(actionType, p_locationOfOrigin, a3, p_directionOfMovementInputWorldSpace, a6, a7, a8, a9, p_player, p_previousAction_mb);
    *p_newAction_out = newAction;
    if (!newAction) return false;
    // ENFORCE-52: swap before type-specific setup (same reasoning as above).
    RunPostConstruct(*newAction);
    if (p_player)
        AvailableParkourAction__InitializePlayerRef(&newAction->playerRef, p_player);
    GET_AND_CAST_FANCY_FUNC(*newAction, ParkourActionKnownFancyVFuncs::Set2FloatsAfterCreation)(newAction, a13, p_epsilon_mb);
    bool isActionFits = GET_AND_CAST_FANCY_FUNC(*newAction, ParkourActionKnownFancyVFuncs::InitialTestIfActionFits)(
        newAction,
        p_locationOfOrigin,
        a3,
        p_directionOfMovementInputWorldSpace,
        a5,
        a6,
        a9,
        p_player,
        p_previousAction_mb);
    bool isDiscarded = !isActionFits;
    RunCreationDiscardOverrides(*newAction, isDiscarded);
    std::shared_ptr<ParkourCycleLogged> currentCycle = GetCurrentLoggedParkourCycle();
    currentCycle->LogActionInitialCreation(*newAction, isDiscarded);
    currentCycle->m_LocationOfOrigin = *(Vector3f*)p_locationOfOrigin;
    currentCycle->m_DirectionOfMovementInputWorldSpace = *(Vector3f*)p_directionOfMovementInputWorldSpace;
    if (isDiscarded)
    {
        newAction->Unk008_Destroy(0);
        ACU::Memory::ACUDeallocateBytes((byte*)newAction);
        *p_newAction_out = nullptr;
        return false;
    }
    return true;
}

// ── Sort&select replacement (verbatim pipeline from ACUFixes-0.9.3) ──────────
// Replaces SortAndSelectBestMatchingAction at its function start. Logging goes
// through ParkourLog; ParkourLogStubs.cpp provides pass-through stubs so the
// runtime behavior equals the vanilla pipeline apart from the callbacks.

constexpr int RESULT_OF_PARKOUR_SORT_AND_SELECT__NO_ACTIONS_ACCEPTED = -1;

static std::optional<int> FindIndexForAction(AvailableParkourAction& action, SmallArray<AvailableParkourAction*>& p_parkourSensorsResults)
{
    auto foundIt = std::find(p_parkourSensorsResults.begin(), p_parkourSensorsResults.end(), &action);
    if (foundIt != p_parkourSensorsResults.end())
    {
        return foundIt - p_parkourSensorsResults.begin();
    }
    return {};
}

int SortAndSelectBestMatchingAction_FullReplacement_inner(
    ParkourTester* parkourTester
    , __m128* p_locationOfOrigin
    , uint64 a3
    , __m128* p_directionOfMovementInputWorldSpace
    , float a5
    , int a6
    , char a7
    , __int64 a8
    , SmallArray<AvailableParkourAction*>& p_parkourSensorsResults
    , std::shared_ptr<ParkourCycleLogged>& currentCycle
)
{
    // Memory-sanity gate: drop candidates whose fancyVTable pointer is not a
    // plausible game fancy-table BEFORE any dispatch (fitness reads, type
    // reads, subscriber callbacks). A single poisoned candidate otherwise
    // crashes the first fancy-fn call, regardless of which plugin or hotkey
    // produced the bad pointer. Also require a callable GetFitness — the
    // weights comparator below calls it repeatedly under std::sort and is
    // otherwise the wild-call site.
    for (int i = 0; i < p_parkourSensorsResults.size; i++)
    {
        AvailableParkourAction* action = p_parkourSensorsResults[i];
        const uint64 table = (action && IsReadableMemory((uint64)action, 0x10))
            ? (uint64)action->fancyVTable : 0;
        if (table >= 0x140000000 && table < 0x148000000
            && IsPlausibleFancyTable(table)
            && GetCallableFancyFn(table, ParkourActionKnownFancyVFuncs::GetFitness::idx))
            continue;
        SmallArray_POD__RemoveGeneric(&p_parkourSensorsResults, i--, 8u);
    }

    currentCycle->LogActionsBeforeFiltering(p_parkourSensorsResults);

    auto& allParkourCallbacks = GenericHooksInParkourFiltering::GetSingleton()->m_Callbacks;
    for (ParkourCallbacks* parkourCallbacks : allParkourCallbacks)
    {
        if (!parkourCallbacks->ChooseBeforeFiltering_fnp) continue;
        AvailableParkourAction* selectedBeforeFiltering = parkourCallbacks->ChooseBeforeFiltering_fnp(parkourCallbacks->m_UserData, p_parkourSensorsResults);
        if (selectedBeforeFiltering)
        {
            if (std::optional<int> idx = FindIndexForAction(*selectedBeforeFiltering, p_parkourSensorsResults))
            {
                return *idx;
            }
        }
    }
    if (p_parkourSensorsResults.size > 0)
    {
        for (int i = 0; i < p_parkourSensorsResults.size; i++)
        {
            AvailableParkourAction* action = p_parkourSensorsResults[i];
            float fitness = GET_AND_CAST_FANCY_FUNC(*action, ParkourActionKnownFancyVFuncs::GetFitness)(action);
            bool isDiscarded_becauseFitnessWeightTooLow = fabsf(fitness) <= 0.0000099999997;
            currentCycle->LogActionBeforeFiltering(*action, fitness, isDiscarded_becauseFitnessWeightTooLow);
            // ENFORCE-52: keep type-52 candidates in the array even at ~zero
            // fitness (a failed wallrun's fitness is ~0 by nature — the
            // weight boost happens later, at the sort stage, so this cull
            // would erase every enforced candidate before force-select).
            if (isDiscarded_becauseFitnessWeightTooLow && !FVTS_ShouldSkipFitnessCull(action))
            {
                SmallArray_POD__RemoveGeneric(&p_parkourSensorsResults, i--, 8u);
            }
        }
    }
    {
        // Idk what this call is, doesn't seem to do anything. (ACUFixes-0.9.3)
        __m128 smthOut;
        GET_AND_CAST_FANCY_FUNC(*parkourTester, ParkourTesterKnownFancyVFuncs::ParkourTester_FancyVFunc_0x16)(&smthOut, *parkourTester, p_locationOfOrigin, p_directionOfMovementInputWorldSpace);
    }
    {
        struct ActionWeights
        {
            float fitness, defaultWeight, totalWeight;
        };
        auto GetActionWeights = [](ParkourTester& parkourTester, AvailableParkourAction& action) -> ActionWeights {
            float fitness = GET_AND_CAST_FANCY_FUNC(action, ParkourActionKnownFancyVFuncs::GetFitness)(&action);
            float defaultWeight = GET_AND_CAST_FANCY_FUNC(parkourTester, ParkourTesterKnownFancyVFuncs::GetDefaultWeightForAction)(parkourTester, action);
            float totalWeight = fitness * defaultWeight;

            return { fitness, defaultWeight, totalWeight };
            };
        auto SortByDescendingTotalFitness = [&](AvailableParkourAction* actionFirst, AvailableParkourAction* action2nd) {
            ActionWeights weights1st = GetActionWeights(*parkourTester, *actionFirst);
            ActionWeights weights2nd = GetActionWeights(*parkourTester, *action2nd);
            weights1st.totalWeight = currentCycle->LogActionWeights(*actionFirst, weights1st.defaultWeight, weights1st.totalWeight);
            weights2nd.totalWeight = currentCycle->LogActionWeights(*action2nd, weights2nd.defaultWeight, weights2nd.totalWeight);
            return weights1st.totalWeight > weights2nd.totalWeight;
            };
        std::sort(p_parkourSensorsResults.begin(), p_parkourSensorsResults.end(), SortByDescendingTotalFitness);
    }
    std::optional<int> selectedBestMatch;
    for (uint16 i = 0; i < p_parkourSensorsResults.size; i++)
    {
        AvailableParkourAction* action = p_parkourSensorsResults[i];
        const bool isPassedFinalFilter1 = AvailableParkourAction__FinalFilter1(action, p_locationOfOrigin, a8, parkourTester->entity);
        currentCycle->LogActionFinalFilter1(*p_parkourSensorsResults[i], isPassedFinalFilter1);
        if (!isPassedFinalFilter1) continue;
        const bool isPassedFinalFilter2 = AvailableParkourAction__FinalFilter2(action, parkourTester->entity, a8);
        currentCycle->LogActionFinalFilter2(*p_parkourSensorsResults[i], isPassedFinalFilter2);
        if (!isPassedFinalFilter2) continue;

        // The action with the greatest "totalWeight" that passes these
        // preceding two tests is selected.
        selectedBestMatch = i;
        break;
    }

    for (ParkourCallbacks* parkourCallbacks : allParkourCallbacks)
    {
        if (!parkourCallbacks->ChooseAfterSorting_fnp) continue;
        AvailableParkourAction* selectedAfterSorting =
            parkourCallbacks->ChooseAfterSorting_fnp(parkourCallbacks->m_UserData, p_parkourSensorsResults, selectedBestMatch ? p_parkourSensorsResults[*selectedBestMatch] : nullptr);
        if (selectedAfterSorting)
        {
            if (std::optional<int> idx = FindIndexForAction(*selectedAfterSorting, p_parkourSensorsResults))
            {
                selectedBestMatch = idx;
                break;
            }
        }
    }

    int result = selectedBestMatch ? *selectedBestMatch : RESULT_OF_PARKOUR_SORT_AND_SELECT__NO_ACTIONS_ACCEPTED;

    return result;
}

// SEH guard around the whole selection pipeline. The surviving wild-call class
// (crash inside the game's own FinalFilter1 — e.g. under FreeJump's dynamically
// installed fallback cave — on a candidate whose table/internal data is stale)
// cannot be caught by validating pointers from outside: the game's fn itself
// dispatches slots we can't enumerate. Swallow the exception, skip the cycle,
// count it (UI + debug output). The wrapper must stay free of C++ objects that
// need unwinding for __try to be legal.
static std::atomic<int> g_SortPipelineExceptions{ 0 };
int SortPipeline_ExceptionCount()
{
    return g_SortPipelineExceptions.load(std::memory_order_relaxed);
}

static int SortPipeline_LogException(unsigned int code, EXCEPTION_POINTERS*)
{
    g_SortPipelineExceptions.fetch_add(1, std::memory_order_relaxed);
    char buf[64];
    sprintf_s(buf, "FVTS: pipeline exception swallowed (code 0x%X)\n", code);
    OutputDebugStringA(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int SortAndSelect_Guarded(
    ParkourTester* parkourTester,
    __m128* p_locationOfOrigin,
    uint64 a3,
    __m128* p_directionOfMovementInputWorldSpace,
    float a5,
    int a6,
    char a7,
    __int64 a8,
    SmallArray<AvailableParkourAction*>& p_parkourSensorsResults,
    std::shared_ptr<ParkourCycleLogged>* currentCycle)
{
    __try
    {
        return SortAndSelectBestMatchingAction_FullReplacement_inner(
            parkourTester, p_locationOfOrigin, a3, p_directionOfMovementInputWorldSpace,
            a5, a6, a7, a8, p_parkourSensorsResults, *currentCycle);
    }
    __except (SortPipeline_LogException(GetExceptionCode(), GetExceptionInformation()))
    {
        return RESULT_OF_PARKOUR_SORT_AND_SELECT__NO_ACTIONS_ACCEPTED;
    }
}

int SortAndSelectBestMatchingAction_FullReplacement(
    ParkourTester* parkourTester,
    __m128* p_locationOfOrigin,
    uint64 a3,
    __m128* p_directionOfMovementInputWorldSpace,
    float a5,
    int a6,
    char a7,
    __int64 a8,
    SmallArray<AvailableParkourAction*>& p_parkourSensorsResults)
{
    std::shared_ptr<ParkourCycleLogged> currentCycle = GetCurrentLoggedParkourCycle();
    int selectedIdx = SortAndSelect_Guarded(
        parkourTester,
        p_locationOfOrigin,
        a3, p_directionOfMovementInputWorldSpace,
        a5, a6, a7, a8,
        p_parkourSensorsResults,
        &currentCycle
    );
    if (selectedIdx != RESULT_OF_PARKOUR_SORT_AND_SELECT__NO_ACTIONS_ACCEPTED)
    {
        AvailableParkourAction* selectedAction = p_parkourSensorsResults[selectedIdx];
        currentCycle->LogActionWhenReturningBestMatch(*selectedAction);
    }
    return selectedIdx;
}

GPH_Creation::GPH_Creation()
{
    // RE-ENABLED for FailedWallRunPlugin. Both addresses are the
    // ACUFixes-0.9.3 / 1.5.0-verified set, confirmed present in the two
    // reference DLLs that demonstrably work on this install (ACUFixes.dll and
    // ACU_ParkourPatches.dll — see PROGRESS.md session 6 evidence), and
    // FailedWallRunPlugin.cpp independently dump-verifies 0x1401D1CF0 against
    // the runtime dump. The creation-stage replacement is what invokes
    // OverrideCreationDiscard_fnp (keep-alive + 53->52 conversion).
    // NOTE: like the sort&select start, these are function-start replacements;
    // do not let two plugins replace the same address simultaneously.
    PresetScript_ReplaceFunctionAtItsStart(0x1401D1CF0, (void*)&CreateParkourActionAndPerformInitialTestIfFits_A_FullReplacement);
    PresetScript_ReplaceFunctionAtItsStart(0x1401D1E50, (void*)&CreateParkourActionAndPerformInitialTestIfFits_B_FullReplacement);
}

GPH_SortAndSelect::GPH_SortAndSelect()
{
    // Function-start replacement of SortAndSelectBestMatchingAction.
    // 0x140133B00 is the ACUFixes-0.9.3 / 1.5.0 value, confirmed by the
    // constant scan of ACUFixes.dll and ACU_ParkourPatches.dll (both working
    // on this install hook this exact address). A function-start replacement
    // needs no stolen-byte relocation, unlike the previous mid-function hook.
    PresetScript_ReplaceFunctionAtItsStart(0x140133B00, (void*)&SortAndSelectBestMatchingAction_FullReplacement);
}

// ── SharedHookActivator ──────────────────────────────────────────────────────

SharedHookActivator::SharedHookActivator(std::function<void()> onCreate, std::function<void()> onDestroy)
    : m_OnCreate(onCreate), m_OnDestroy(onDestroy) { m_OnCreate(); }

SharedHookActivator::~SharedHookActivator() { m_OnDestroy(); }

// ── GenericHooksInParkourFiltering ────────────────────────────────────────────

std::shared_ptr<GenericHooksInParkourFiltering>& GenericHooksInParkourFiltering::GetSingleton()
{
    static auto s = std::make_shared<GenericHooksInParkourFiltering>();
    return s;
}

void GenericHooksInParkourFiltering::Subscribe(ParkourCallbacks& cb)
{
    auto it = m_Callbacks.begin();
    for (; it != m_Callbacks.end(); ++it)
        if (cb.m_CallbackPriority < (*it)->m_CallbackPriority) break;
    m_Callbacks.insert(it, &cb);
}

void GenericHooksInParkourFiltering::Unsubscribe(ParkourCallbacks& cb)
{
    m_Callbacks.erase(std::remove(m_Callbacks.begin(), m_Callbacks.end(), &cb), m_Callbacks.end());
}

std::shared_ptr<SharedHookActivator> GenericHooksInParkourFiltering::RequestGPHCreation()
{
    std::lock_guard lk{ m_Mutex };
    auto a = m_Activator_GPHCreation.lock();
    if (a) return a;
    a = std::make_shared<SharedHookActivator>(
        [&]() { gph_creation.Activate(); },
        [&]() { gph_creation.Deactivate(); });
    m_Activator_GPHCreation = a;
    return a;
}

std::shared_ptr<SharedHookActivator> GenericHooksInParkourFiltering::RequestGPHSortAndSelect()
{
    std::lock_guard lk{ m_Mutex };
    auto a = m_Activator_GPHSortAndSelect.lock();
    if (a) return a;
    a = std::make_shared<SharedHookActivator>(
        [&]() { gph_sortAndSelect.Activate(); },
        [&]() { gph_sortAndSelect.Deactivate(); });
    m_Activator_GPHSortAndSelect = a;
    return a;
}
