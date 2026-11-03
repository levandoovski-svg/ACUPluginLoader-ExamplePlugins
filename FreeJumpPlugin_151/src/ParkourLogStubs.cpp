#include "pch.h"
#include "ParkourDebugging/ParkourLog.h"
#include <memory>

ParkourActionLogged::ParkourActionLogged(AvailableParkourAction& action, size_t indexInOrderOfCreationInCycle)
{
    (void)action;
    (void)indexInOrderOfCreationInCycle;
}

void ParkourCycleLogged::LogActionInitialCreation(AvailableParkourAction& newAction, bool& isDiscarded_immediatelyAfterCreation)
{
    (void)newAction;
    isDiscarded_immediatelyAfterCreation = false;
}

void ParkourCycleLogged::LogActionsBeforeFiltering(SmallArray<AvailableParkourAction*>& allActionsBeforeFiltering)
{
    (void)allActionsBeforeFiltering;
}

void ParkourCycleLogged::LogActionBeforeFiltering(AvailableParkourAction& action, float fitness, bool& isDiscarded_becauseFitnessWeightTooLow)
{
    (void)action;
    (void)fitness;
    isDiscarded_becauseFitnessWeightTooLow = false;
}

float ParkourCycleLogged::LogActionWeights(AvailableParkourAction& action, float defaultWeight, float totalWeight)
{
    (void)action;
    (void)defaultWeight;
    return totalWeight;
}

void ParkourCycleLogged::LogActionFinalFilter1(AvailableParkourAction& action, bool resultOfFinalFilter1)
{
    (void)action;
    (void)resultOfFinalFilter1;
}

void ParkourCycleLogged::LogActionFinalFilter2(AvailableParkourAction& action, bool resultOfFinalFilter2)
{
    (void)action;
    (void)resultOfFinalFilter2;
}

void ParkourCycleLogged::LogActionWhenReturningBestMatch(AvailableParkourAction& bestMatchMove)
{
    (void)bestMatchMove;
}

ParkourActionLogged& ParkourCycleLogged::GetOrMakeRecordForAction(AvailableParkourAction& action)
{
    static ParkourActionLogged dummy(action, 0);
    return dummy;
}

ParkourActionLogged& ParkourCycleLogged::MakeRecordForAction(AvailableParkourAction& action)
{
    static ParkourActionLogged dummy(action, 0);
    return dummy;
}

std::shared_ptr<ParkourCycleLogged> GetCurrentLoggedParkourCycle()
{
    static ParkourCycleLogged dummy(0);
    static auto ptr = std::shared_ptr<ParkourCycleLogged>(&dummy, [](ParkourCycleLogged*) {});
    return ptr;
}
