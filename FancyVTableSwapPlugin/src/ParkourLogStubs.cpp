// ParkourLog stubs for the passive discarder plugin.
// The passive plugin never reads parkour records (no force-select, no weight
// boost, no diagnostics window), and record-keeping allocated one record +
// mutex lock per created action per cycle - a real chunk of the lag measured
// with the overrider armed. GPH still calls these hooks, so everything here
// routes to the DISABLED dummy cycle: every LogAction* becomes a no-op via
// ParkourCycleLogged::m_IsDisabled and no cycle or record is ever allocated.
#include "pch.h"
#include "ParkourDebugging/ParkourLog.h"
#include <memory>
#include <mutex>

ParkourActionLogged::ParkourActionLogged(AvailableParkourAction& action, size_t indexInOrderOfCreationInCycle)
    : m_ActionPtr(&action)
    , m_FancyVTable((uint64)action.fancyVTable)
    , m_ActionType((EnumParkourAction)0)
    , m_Index(indexInOrderOfCreationInCycle)
{}

void ParkourCycleLogged::LogActionInitialCreation(AvailableParkourAction& newAction, bool& isDiscarded_immediatelyAfterCreation)
{
    (void)newAction; (void)isDiscarded_immediatelyAfterCreation;
}
void ParkourCycleLogged::LogActionsBeforeFiltering(SmallArray<AvailableParkourAction*>& allActionsBeforeFiltering)
{
    (void)allActionsBeforeFiltering;
}
void ParkourCycleLogged::LogActionBeforeFiltering(AvailableParkourAction& action, float fitness, bool& isDiscarded_becauseFitnessWeightTooLow)
{
    (void)action; (void)fitness; (void)isDiscarded_becauseFitnessWeightTooLow;
}
float ParkourCycleLogged::LogActionWeights(AvailableParkourAction& action, float defaultWeight, float totalWeight)
{
    (void)action; (void)defaultWeight;
    return totalWeight; // no weight boost - the game ranks candidates itself
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
    // Unreachable through the disabled dummy cycle; make it harmless anyway.
    return *m_Actions.emplace_back(new ParkourActionLogged(action, m_IndexOfNextLoggedAction++));
}
ParkourActionLogged& ParkourCycleLogged::MakeRecordForAction(AvailableParkourAction& action)
{
    return GetOrMakeRecordForAction(action);
}

bool ParkourLog::IsActionShouldBeDisplayed(ParkourActionLogged& action)
{
    return !action.m_IsDiscarded_immediatelyAfterCreation;
}
void ParkourLog::DrawDisplayControls() {}
ParkourLog::ParkourLog()
    : m_DisabledDummyCycle(new ParkourCycleLogged())
{}
std::shared_ptr<ParkourCycleLogged> ParkourLog::GetCurrentLoggedParkourCycle()
{
    return m_DisabledDummyCycle; // all LogAction* no-op on the disabled dummy
}
std::shared_ptr<ParkourCycleLogged> ParkourLog::GetLatestLoggedParkourCycle()
{
    return m_DisabledDummyCycle;
}
std::vector<std::shared_ptr<ParkourCycleLogged>> ParkourLog::GetRecentCycles()
{
    return {};
}
std::shared_ptr<ParkourCycleLogged> GetCurrentLoggedParkourCycle()
{
    return ParkourLog::GetSingleton().GetCurrentLoggedParkourCycle();
}
