// Real-lite port of ACUFixes' ParkourDebugging scanner (ParkourLog).
// Formerly full no-op stubs — which silently threw away every scanned parkour
// cycle AND (worse) forced isDiscarded=false at creation, mutating vanilla
// behavior. This file keeps only the parts this plugin needs:
//   - every created action logged (ptr, creation vtable, type, discard verdict)
//   - cycle history (64 cycles, timestamped by GetTickCount64 instead of the
//     World clock so we don't reach into game singletons)
// Not ported: the ImGui debugger window, console/3D logging, EnforcedMove,
// weight/filter re-logging. If this ever grows a full debugger UI, port the
// real ParkourLog.cpp instead.
#include "pch.h"
#include "ParkourDebugging/ParkourLog.h"
#include "ParkourDebugging/GetTypeIntSafe.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include <memory>
#include <mutex>

// Defined in FancyVTableSwapPlugin.cpp — true while the failed-wallrun
// enforcer is armed (hotkey active + Enforce52 checkbox).
bool FVTS_IsEnforcing();

ParkourActionLogged::ParkourActionLogged(AvailableParkourAction& action, size_t indexInOrderOfCreationInCycle)
    : m_ActionPtr(&action)
    , m_FancyVTable((uint64)action.fancyVTable)
    , m_ActionType((EnumParkourAction)GetTypeIntSafe(&action))
    , m_Index(indexInOrderOfCreationInCycle)
{}

void ParkourCycleLogged::LogActionInitialCreation(AvailableParkourAction& newAction, bool& isDiscarded_immediatelyAfterCreation)
{
    if (m_IsDisabled) return;
    std::lock_guard _lock{ m_Mutex };
    m_IsSortingDirty = true;
    ParkourActionLogged& record = *m_Actions.emplace_back(
        std::make_unique<ParkourActionLogged>(newAction, m_IndexOfNextLoggedAction++));
    record.m_IsDiscarded_immediatelyAfterCreation = isDiscarded_immediatelyAfterCreation;
}
void ParkourCycleLogged::LogActionsBeforeFiltering(SmallArray<AvailableParkourAction*>& allActionsBeforeFiltering)
{
    (void)allActionsBeforeFiltering;
}
void ParkourCycleLogged::LogActionBeforeFiltering(AvailableParkourAction& action, float fitness, bool& isDiscarded_becauseFitnessWeightTooLow)
{
    (void)action; (void)fitness;
    (void)isDiscarded_becauseFitnessWeightTooLow;
}
float ParkourCycleLogged::LogActionWeights(AvailableParkourAction& action, float defaultWeight, float totalWeight)
{
    (void)defaultWeight;
    // Enforce-52 stage 2: while the failed-wallrun enforcer is armed, give
    // every type-52 candidate an unbeatable weight so it survives the
    // "fitness too low" removal and wins the sort.
    if (!m_IsDisabled && FVTS_IsEnforcing()
        && GetTypeIntSafe(&action) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb)
        return totalWeight = 1e9f;
    return totalWeight;
}
void ParkourCycleLogged::LogActionFinalFilter1(AvailableParkourAction& action, bool resultOfFinalFilter1)
{
    (void)action; (void)resultOfFinalFilter1;
}
void ParkourCycleLogged::LogActionFinalFilter2(AvailableParkourAction& action, bool resultOfFinalFilter2)
{
    (void)action; (void)resultOfFinalFilter2;
}
void ParkourCycleLogged::LogActionWhenReturningBestMatch(AvailableParkourAction& bestMatchMove)
{
    (void)bestMatchMove;
}
ParkourActionLogged& ParkourCycleLogged::GetOrMakeRecordForAction(AvailableParkourAction& action)
{
    std::lock_guard _lock{ m_Mutex };
    m_IsSortingDirty = true;
    for (auto& record : m_Actions)
    {
        if (record->m_ActionPtr != &action) continue;
        if (record->m_IsDiscarded_immediatelyAfterCreation) continue;
        return *record;
    }
    return *m_Actions.emplace_back(std::make_unique<ParkourActionLogged>(action, m_IndexOfNextLoggedAction++));
}
ParkourActionLogged& ParkourCycleLogged::MakeRecordForAction(AvailableParkourAction& action)
{
    std::lock_guard _lock{ m_Mutex };
    return *m_Actions.emplace_back(std::make_unique<ParkourActionLogged>(action, m_IndexOfNextLoggedAction++));
}

// shortcut: GetTickCount64 instead of the World clock — cycles may merge across
// two real cycles within the same millisecond; harmless here (we only read the
// latest cycle), upgrade to the World clock if cycle-splitting becomes wrong.
namespace {
struct PluginParkourLog
{
    std::vector<std::shared_ptr<ParkourCycleLogged>> m_Cycles;
    std::mutex m_Mutex;
    std::shared_ptr<ParkourCycleLogged> m_DisabledDummy;

    std::shared_ptr<ParkourCycleLogged>& PushNew(uint64 timestamp)
    {
        constexpr size_t k_MaxRetainCycles = 64;
        if (m_Cycles.size() >= k_MaxRetainCycles)
            m_Cycles.erase(m_Cycles.begin(), m_Cycles.begin() + (m_Cycles.size() - k_MaxRetainCycles + 1));
        return m_Cycles.emplace_back(std::make_shared<ParkourCycleLogged>(timestamp));
    }
    std::shared_ptr<ParkourCycleLogged> GetCurrent()
    {
        uint64 now = GetTickCount64();
        std::lock_guard _lock{ m_Mutex };
        if (!m_Cycles.size() || m_Cycles.back()->m_Timestamp != now)
            return PushNew(now);
        return m_Cycles.back();
    }
    std::shared_ptr<ParkourCycleLogged> GetLatest()
    {
        std::lock_guard _lock{ m_Mutex };
        if (!m_Cycles.size()) return {};
        return m_Cycles.back();
    }
};
static PluginParkourLog& GetPluginParkourLog()
{
    static PluginParkourLog singleton;
    return singleton;
}
} // namespace

ParkourLog::ParkourLog()
    : m_DisabledDummyCycle(new ParkourCycleLogged())
{}
bool ParkourLog::IsActionShouldBeDisplayed(ParkourActionLogged& action)
{
    return !action.m_IsDiscarded_immediatelyAfterCreation;
}
void ParkourLog::DrawDisplayControls() {}
std::shared_ptr<ParkourCycleLogged> ParkourLog::GetCurrentLoggedParkourCycle()
{
    return GetPluginParkourLog().GetCurrent();
}
std::shared_ptr<ParkourCycleLogged> ParkourLog::GetLatestLoggedParkourCycle()
{
    return GetPluginParkourLog().GetLatest();
}
std::vector<std::shared_ptr<ParkourCycleLogged>> ParkourLog::GetRecentCycles()
{
    std::lock_guard _lock{ GetPluginParkourLog().m_Mutex };
    return GetPluginParkourLog().m_Cycles;
}
std::shared_ptr<ParkourCycleLogged> GetCurrentLoggedParkourCycle()
{
    return ParkourLog::GetSingleton().GetCurrentLoggedParkourCycle();
}
