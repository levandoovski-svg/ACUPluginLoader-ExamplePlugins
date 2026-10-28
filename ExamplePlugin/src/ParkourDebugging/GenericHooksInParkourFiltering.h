#pragma once

// ============================================================================
//  PROJECT-LOCAL SHADOW of ACUFixes-0.9.3's
//  ParkourDebugging/GenericHooksInParkourFiltering.h
//  ----------------------------------------------------------------------------
//  This file shadows the reference header because $(ProjectDir)src comes first
//  in the include path of both ExamplePlugin projects. The ONLY difference from
//  the ACUFixes-0.9.3 original is the added ParkourCallbacks::
//  OverrideCreationDiscard_fnp member (default nullptr, so existing callbacks
//  are unaffected). Do not change anything else — everything here must stay
//  layout- and semantics-identical to the reference header.
//
//  Why: FailedWallRunPlugin needs a creation-stage hook that can keep an action
//  alive after the vanilla InitialTestIfActionFits verdict, and convert
//  wallrunUpFromGround_mb (53) into wallrunUpFromGroundFailed_mb (52) via a
//  fancy-vtable swap. The old plugin binary used a framework that exposed an
//  OverrideCreationDiscard_fnp callback; that callback no longer exists in the
//  stock 0.9.3 header, so it is restored here (see
//  src/GenericHooksInParkourFiltering.cpp for the creation-stage replacements
//  that invoke it).
// ============================================================================

#include "ACU/SmallArray.h"

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"

struct GPH_Creation : AutoAssemblerCodeHolder_Base
{
    GPH_Creation();
};
struct GPH_SortAndSelect : AutoAssemblerCodeHolder_Base
{
    GPH_SortAndSelect();
};

// Used as basically a refcount for code hooks.
// A particular hook will be installed/Activated while an associated instance
// of activator is alive.
struct SharedHookActivator
{
    std::function<void()> m_OnCreate;
    std::function<void()> m_OnDestroy;

    SharedHookActivator(std::function<void()> onCreate, std::function<void()> onDestroy);
    ~SharedHookActivator();

    SharedHookActivator(const SharedHookActivator& rhs) = delete;
    SharedHookActivator& operator=(const SharedHookActivator& rhs) = delete;
};

class AvailableParkourAction;
struct ParkourCallbacks
{
    AvailableParkourAction* (*ChooseBeforeFiltering_fnp)(void* userData, SmallArray<AvailableParkourAction*>& actions) = nullptr;
    AvailableParkourAction* (*ChooseAfterSorting_fnp)(void* userData, SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame) = nullptr;
    // Creation-stage override. Invoked from the creation-wrapper replacements
    // for every successfully constructed action, BEFORE the discard decision:
    //   return true  -> the action is kept even if InitialTestIfActionFits
    //                   rejected it (the callback may have mutated it in place);
    //   return false -> no override, vanilla discard verdict stands.
    bool (*OverrideCreationDiscard_fnp)(void* userData, AvailableParkourAction& action) = nullptr;
    void* m_UserData = nullptr;
    float m_CallbackPriority;
    const char* m_Name = nullptr;
};
class GenericHooksInParkourFiltering
{
public:
    AutoAssembleWrapper<GPH_Creation> gph_creation;
    AutoAssembleWrapper<GPH_SortAndSelect> gph_sortAndSelect;

    static std::shared_ptr<GenericHooksInParkourFiltering>& GetSingleton();
    std::shared_ptr<SharedHookActivator> RequestGPHCreation();
    std::shared_ptr<SharedHookActivator> RequestGPHSortAndSelect();

    void Subscribe(ParkourCallbacks& callbacks);
    void Unsubscribe(ParkourCallbacks& callbacks);
    std::vector<ParkourCallbacks*> m_Callbacks;

private:
    std::weak_ptr<SharedHookActivator> m_Activator_GPHCreation;
    std::weak_ptr<SharedHookActivator> m_Activator_GPHSortAndSelect;
    std::mutex m_Mutex;
};
