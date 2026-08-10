#include "pch.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include "ACU/basic_types.h"
#include "ACU/Memory/ACUAllocs.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ParkourDebugging/ParkourTester.h"
#include "ACU_DefineNativeFunction.h"
#include <mutex>
#include <algorithm>

EnumParkourAction AvailableParkourAction::GetEnumParkourAction()
{
    return GET_AND_CAST_FANCY_FUNC(*this, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)(this);
}

DEFINE_GAME_FUNCTION(AvailableParkourAction__FinalFilter1, 0x1401D4360, bool, __fastcall, (AvailableParkourAction*, __m128*, uint64, Entity*));
DEFINE_GAME_FUNCTION(AvailableParkourAction__FinalFilter2, 0x1401D2580, bool, __fastcall, (AvailableParkourAction*, Entity*, __int64));
DEFINE_GAME_FUNCTION(ConstructParkourAction_A, 0x1401CBF00, AvailableParkourAction*, __fastcall, (EnumParkourAction, __m128*, __m128*, __m128*, int, char, __int64, AvailableParkourAction*));
DEFINE_GAME_FUNCTION(ConstructParkourAction_B, 0x1401D1530, AvailableParkourAction*, __fastcall, (EnumParkourAction, __m128*, __m128*, __m128*, int, char, __int64, __int64, Entity*, AvailableParkourAction*));
DEFINE_GAME_FUNCTION(AvailableParkourAction__InitializePlayerRef, 0x140159940, void, __fastcall, (PlayerRefInParkourAction*, Entity*));
DEFINE_GAME_FUNCTION(SmallArray_POD__RemoveGeneric, 0x142726000, void, __fastcall, (void*, int, unsigned int));

class Entity;
struct CollisionProbeForParkour_mb
{
    Vector4f locationAnchorDest;
    Vector4f direction;
    char pad_0020[20];
    float flt_34;
    uint32 collisionLayer_mb;
    char pad_003C[4];
    Vector4f pos40;
    Vector4f pos50;
    PlayerRefInParkourAction shared_buildingEntity;
    char pad_0078[8];
};

// ── Parkour action creation replacements ──────────────────────────────────────

static bool CreateParkourAction_A_Replacement(
    EnumParkourAction actionType, __m128* loc, __m128* a3, __m128* dir,
    float a5, int a6, char a7, CollisionProbeForParkour_mb* probe,
    Entity* player, AvailableParkourAction* prevAction,
    AvailableParkourAction** outAction, float a12, float eps)
{
    AvailableParkourAction* act = ConstructParkourAction_A(actionType, loc, a3, dir, a6, a7, (uint64)probe, prevAction);
    *outAction = act;
    if (!act) return false;
    if (player) AvailableParkourAction__InitializePlayerRef(&act->playerRef, player);
    GET_AND_CAST_FANCY_FUNC(*act, ParkourActionKnownFancyVFuncs::Set2FloatsAfterCreation)(act, a12, eps);
    bool fits = GET_AND_CAST_FANCY_FUNC(*act, ParkourActionKnownFancyVFuncs::InitialTestIfActionFits)(act, loc, a3, dir, a5, a6, (uint64)probe, player, prevAction);
    if (!fits) {
        act->Unk008_Destroy(0);
        ACU::Memory::ACUDeallocateBytes((byte*)act);
        *outAction = nullptr;
        return false;
    }
    return true;
}

static bool CreateParkourAction_B_Replacement(
    EnumParkourAction actionType, __m128* loc, __m128* a3, __m128* dir,
    float a5, int a6, char a7, uint64 a8, uint64 a9,
    Entity* player, AvailableParkourAction* prevAction,
    AvailableParkourAction** outAction, float a13, float eps)
{
    AvailableParkourAction* act = ConstructParkourAction_B(actionType, loc, a3, dir, a6, a7, a8, a9, player, prevAction);
    *outAction = act;
    if (!act) return false;
    if (player) AvailableParkourAction__InitializePlayerRef(&act->playerRef, player);
    GET_AND_CAST_FANCY_FUNC(*act, ParkourActionKnownFancyVFuncs::Set2FloatsAfterCreation)(act, a13, eps);
    bool fits = GET_AND_CAST_FANCY_FUNC(*act, ParkourActionKnownFancyVFuncs::InitialTestIfActionFits)(act, loc, a3, dir, a5, a6, a9, player, prevAction);
    if (!fits) {
        act->Unk008_Destroy(0);
        ACU::Memory::ACUDeallocateBytes((byte*)act);
        *outAction = nullptr;
        return false;
    }
    return true;
}

// ── GPH hook placements ──────────────────────────────────────────────────────

GPH_Creation::GPH_Creation()
{
    PresetScript_ReplaceFunctionAtItsStart(0x1401D1260, CreateParkourAction_A_Replacement);
    PresetScript_ReplaceFunctionAtItsStart(0x1401D13C0, CreateParkourAction_B_Replacement);
}

static void SortAndSelectHookCallback(AllRegisters* params)
{
    uintptr_t rsp = params->GetRSP();
    auto& actions = *(SmallArray<AvailableParkourAction*>*)(*(uint64*)(rsp + 0x48));
    auto& gphCallbacks = GenericHooksInParkourFiltering::GetSingleton()->m_Callbacks;
    for (auto* cb : gphCallbacks)
        if (cb->ChooseBeforeFiltering_fnp)
            cb->ChooseBeforeFiltering_fnp(cb->m_UserData, actions);
}

GPH_SortAndSelect::GPH_SortAndSelect()
{
    PresetScript_CCodeInTheMiddle(0x14013493E, 7,
        SortAndSelectHookCallback,
        AutoAssemblerCodeHolder_Base::RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
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
