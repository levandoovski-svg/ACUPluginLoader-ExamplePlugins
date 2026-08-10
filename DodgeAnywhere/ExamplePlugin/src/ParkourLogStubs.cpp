#include "pch.h"
#include "ParkourDebugging/ParkourLog.h"   // the real class definitions
#include <memory>

// -------------------------------------------------------------------
// ParkourActionLogged stub constructor
// -------------------------------------------------------------------
ParkourActionLogged::ParkourActionLogged(AvailableParkourAction& action, size_t indexInOrderOfCreationInCycle)
{
    // Just suppress unused parameter warnings; we don't need the real logic.
    (void)action;
    (void)indexInOrderOfCreationInCycle;
}

// -------------------------------------------------------------------
// ParkourCycleLogged member stubs
// -------------------------------------------------------------------

void ParkourCycleLogged::LogActionInitialCreation(AvailableParkourAction& newAction, bool& isDiscarded_immediatelyAfterCreation)
{
    (void)newAction;
    isDiscarded_immediatelyAfterCreation = false;   // safe default
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
    return totalWeight;   // behaviour from the original code
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

// Private helpers (they're called internally by the public stubs above)
ParkourActionLogged& ParkourCycleLogged::GetOrMakeRecordForAction(AvailableParkourAction& action)
{
    // Return a static dummy – never null, never changes.
    static ParkourActionLogged dummy(action, 0);
    return dummy;
}

ParkourActionLogged& ParkourCycleLogged::MakeRecordForAction(AvailableParkourAction& action)
{
    static ParkourActionLogged dummy(action, 0);
    return dummy;
}

// -------------------------------------------------------------------
// Global GetCurrentLoggedParkourCycle() – returns a valid dummy
// -------------------------------------------------------------------
std::shared_ptr<ParkourCycleLogged> GetCurrentLoggedParkourCycle()
{
    static ParkourCycleLogged dummy(0);   // dummy timestamp
    // Return a shared_ptr that does NOT delete the static object.
    static auto ptr = std::shared_ptr<ParkourCycleLogged>(&dummy, [](ParkourCycleLogged*) {});
    return ptr;
}