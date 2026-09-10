#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include "ParkourDebugging/EnumParkourAction.h"

// Callback object subscribed into the existing GenericHooksInParkourFiltering pipeline
// (the same "ChooseBeforeFiltering_fnp" stage used by Request_Spindescent.cpp).
// Instead of force-selecting a move, this one scans every candidate AvailableParkourAction
// each parkour cycle and, for whichever action type you're currently targeting
// (by EnumParkourAction integer value), overrides its distanceMin/distanceMax and
// rejects it (by zeroing its fitness) if the actual computed `distance` for that
// specific candidate falls outside your chosen range.
class ParkourDistanceOverride_Callbacks
{
public:
    ParkourDistanceOverride_Callbacks();

    AvailableParkourAction* ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions);

    ParkourCallbacks m_Callbacks;

    // --- Live-editable via ImGui ---

    // Raw EnumParkourAction value, e.g. 8 == EnumParkourAction::stepDownWaistHeight_mb.
    // See ParkourDebugging/EnumParkourAction.h for the full list.
    int m_TargetActionEnum = (int)EnumParkourAction::stepDownWaistHeight_mb;

    // Any candidate of the target action type whose action->distance falls
    // outside [m_DistanceMin, m_DistanceMax] gets its fitness zeroed out,
    // which makes the game's own selection logic discard it
    // (see SortAndSelectBestMatchingAction_FullReplacement_inner's
    // "isDiscarded_becauseFitnessWeightTooLow" check).
    float m_DistanceMin = 0.0f;
    float m_DistanceMax = 2.0f;

    bool m_LogToConsole = true;
};

// Thin AutoAssemblerCodeHolder_Base wrapper so this can be toggled on/off through
// the usual `AutoAssembleWrapper<T>` + `ImGui::DrawCheckboxForHack(...)` machinery,
// exactly like ParkourActionsExtraProcessing in Request_Spindescent.h/.cpp.
// No actual machine-code hooking is needed here -- we only (Un)Subscribe from the
// already-hooked GenericHooksInParkourFiltering pipeline.
struct Hack_ParkourDistanceOverride : AutoAssemblerCodeHolder_Base
{
    Hack_ParkourDistanceOverride();
    virtual void OnBeforeActivate() override;
    virtual void OnBeforeDeactivate() override;

    // Call this from your existing ImGui hacks window while the hack is active.
    void DrawImGuiControls();

    std::unique_ptr<ParkourDistanceOverride_Callbacks> m_Callbacks;

private:
    std::shared_ptr<GenericHooksInParkourFiltering> m_GPH;
    std::shared_ptr<SharedHookActivator> m_Activator_GPHSortAndSelect;
};
