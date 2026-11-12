#include "pch.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include "AcuPatternScan.h"
#include "MyLog.h"
#include "ACU/basic_types.h"
#include "ACU/Memory/ACUAllocs.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ParkourDebugging/ParkourTester.h"
#include "ParkourDebugging/ParkourLog.h"
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

// These game addresses differ between ACU builds (1.5.0: 0x1401D4DE0 and
// friends, 1.5.1: 0x1401D4360 and friends), but the instructions at them are
// identical, so each site is found by byte signature at startup. A signature
// that misses this build leaves every parkour hook uninstalled and logged,
// instead of calling an address that moved.
static bool (__fastcall* AvailableParkourAction__FinalFilter1)(AvailableParkourAction*, __m128*, uint64, Entity*) = nullptr;
static bool (__fastcall* AvailableParkourAction__FinalFilter2)(AvailableParkourAction*, Entity*, __int64) = nullptr;
static AvailableParkourAction* (__fastcall* ConstructParkourAction_A)(EnumParkourAction, __m128*, __m128*, __m128*, int, char, __int64, AvailableParkourAction*) = nullptr;
static AvailableParkourAction* (__fastcall* ConstructParkourAction_B)(EnumParkourAction, __m128*, __m128*, __m128*, int, char, __int64, __int64, Entity*, AvailableParkourAction*) = nullptr;
static void (__fastcall* AvailableParkourAction__InitializePlayerRef)(PlayerRefInParkourAction*, Entity*) = nullptr;
static void (__fastcall* SmallArray_POD__RemoveGeneric)(void*, int, unsigned int) = nullptr;

static uintptr_t g_CreateWrapperA = 0;   // 1.5.0: 0x1401D1CF0, 1.5.1: 0x1401D1260
static uintptr_t g_CreateWrapperB = 0;   // 1.5.0: 0x1401D1E50, 1.5.1: 0x1401D13C0
static uintptr_t g_SortAndSelectSite = 0; // 1.5.0: 0x140133B00, 1.5.1: 0x140134940

// Resolves every address the replacements call into. All-or-nothing: a partial
// set would let a replacement call a null pointer the first time parkour runs.
static bool ResolveParkourGameAddresses()
{
    static bool resolved = false;
    static bool ok = false;
    if (resolved) return ok;
    resolved = true;

    AvailableParkourAction__FinalFilter1 = (decltype(AvailableParkourAction__FinalFilter1))AcuScan::Find(
        "48 8B 41 08 4C 8B 90 D0 0E 00 00 49 FF E2");
    AvailableParkourAction__FinalFilter2 = (decltype(AvailableParkourAction__FinalFilter2))AcuScan::Find(
        "48 8B 41 08 4C 8B 88 D0 09 00 00 49 FF E1");
    ConstructParkourAction_A = (decltype(ConstructParkourAction_A))AcuScan::Find(
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8B EC 48 83 EC 40 83 C1 FE");
    ConstructParkourAction_B = (decltype(ConstructParkourAction_B))AcuScan::Find(
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 83 C1 CD");
    AvailableParkourAction__InitializePlayerRef = (decltype(AvailableParkourAction__InitializePlayerRef))AcuScan::Find(
        "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 20 48 8B EA 48 8B F9");
    SmallArray_POD__RemoveGeneric = (decltype(SmallArray_POD__RemoveGeneric))AcuScan::Find(
        "48 89 5C 24 08 57 48 83 EC 20 0F B7 59 0A 8D 42 01 45 8B D8 44 8B D2");
    g_CreateWrapperA = AcuScan::Find(
        "48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 50 0F B6 84 24 B0 00 00 00");
    g_CreateWrapperB = AcuScan::Find(
        "4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 8B 84 24 B8 00 00 00");
    g_SortAndSelectSite = AcuScan::Find(
        "48 8B C4 48 89 58 18 48 89 68 20 48 89 50 10 56 57 41 54");

    ok = AvailableParkourAction__FinalFilter1 && AvailableParkourAction__FinalFilter2
        && ConstructParkourAction_A && ConstructParkourAction_B
        && AvailableParkourAction__InitializePlayerRef && SmallArray_POD__RemoveGeneric
        && g_CreateWrapperA && g_CreateWrapperB && g_SortAndSelectSite;
    if (!ok)
        LOG_DEBUG(DefaultLogger, "%s", "[GPH] signature scan missed this ACU build; parkour hooks stay disabled.\n");
    return ok;
}

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
            if (isDiscarded_becauseFitnessWeightTooLow && SmallArray_POD__RemoveGeneric)
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
    int selectedIdx = SortAndSelectBestMatchingAction_FullReplacement_inner(
        parkourTester,
        p_locationOfOrigin,
        a3, p_directionOfMovementInputWorldSpace,
        a5, a6, a7, a8,
        p_parkourSensorsResults,
        currentCycle
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
    // RE-ENABLED for FailedWallRunPlugin. The two creation wrappers are
    // function-start replacements found by signature (see
    // ResolveParkourGameAddresses); if this build's bytes are unknown nothing
    // is installed, so the feature stays inert instead of crashing.
    // NOTE: like the sort&select start, these are function-start replacements;
    // do not let two plugins replace the same address simultaneously.
    if (!ResolveParkourGameAddresses())
        return;
    PresetScript_ReplaceFunctionAtItsStart(g_CreateWrapperA, (void*)&CreateParkourActionAndPerformInitialTestIfFits_A_FullReplacement);
    PresetScript_ReplaceFunctionAtItsStart(g_CreateWrapperB, (void*)&CreateParkourActionAndPerformInitialTestIfFits_B_FullReplacement);
}

GPH_SortAndSelect::GPH_SortAndSelect()
{
    // Function-start replacement of SortAndSelectBestMatchingAction, found by
    // signature: 1.5.0 has it at 0x140133B00, 1.5.1 at 0x140134940. A
    // function-start replacement needs no stolen-byte relocation, unlike the
    // previous mid-function hook that split an instruction and crashed.
    if (!ResolveParkourGameAddresses())
        return;
    PresetScript_ReplaceFunctionAtItsStart(g_SortAndSelectSite, (void*)&SortAndSelectBestMatchingAction_FullReplacement);
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
