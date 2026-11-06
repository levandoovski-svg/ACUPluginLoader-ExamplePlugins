#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include <memory>
#include <vector>
#include <mutex>

class AvailableParkourAction;

// Single-purpose plugin: while the hotkey is ACTIVE, force the failed wallrun
// (wallrunUpFromGroundFailed_mb / type 52) instead of wallrunUpFromGround_mb
// (type 53), regardless of the "no handholds in reach" context:
//   creation: convert 53 -> 52 right after construction (vtable swap at +0x8)
//             and keep it alive despite the InitialTestIfActionFits reject
//   weights:  boost type-52 totalWeight so it wins the sort
//   select:   force-select the (best) type-52 after sorting
class FancyVTableSwapPlugin
{
public:
    FancyVTableSwapPlugin();
    ~FancyVTableSwapPlugin();

    void OnBeforeActivate();
    void OnUpdate();        // per-frame: key + rebind
    void OnImGuiRender();   // ACUPluginLoader menu panel

    // Diagnostics only (no mutations): refresh the discovered-vtable snapshot
    // so the panel stays usable when the sort&select hook runs.
    AvailableParkourAction* ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions);

    // Enforce-52 stages:
    // 0 post-construct: convert 53 -> 52 before any type-specific creation call.
    void OnPostConstruct(AvailableParkourAction& action);
    // 1 creation-discard override: keep (already-converted) 52 alive despite
    // the vanilla "InitialTestIfActionFits" reject.
    bool OnCreationDiscardOverride(AvailableParkourAction& action);
    // 3 force-select: return the best type-52 in the sorted array (prefer one
    // that passed both final filters; the game's own pick is ignored while
    // armed).
    AvailableParkourAction* ChooseAfterSorting(SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame);

    struct DiscoveredVTable
    {
        uint64 vtable = 0;
        int type = -1;
        int count = 0;
    };

private:
    bool IsActive() const;
    void LoadSettings();
    void SaveSettings() const;

    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_Activator;
    // Creation-stage hooks feed the ParkourLog scanner and the conversion.
    std::shared_ptr<SharedHookActivator> m_ActivatorCreation;

    // Config (persisted to INI)
    bool m_Enabled = true;
    int m_Hotkey = 0x77; // F8
    bool m_HoldMode = true;
    bool m_ToggleState = false;
    bool m_WaitingForKey = false;

    // Runtime snapshot (written on game thread, read by UI)
    // shortcut: single snapshot + mutex, no history; fine for diagnostics.
    mutable std::mutex m_Mutex;
    std::vector<DiscoveredVTable> m_Discovered;
    int m_LastActionsSeen = 0;
    bool m_ChooseFired = false;

    // Enforce-52 counters (UI-only).
    int m_EnforceConverted = 0;      // cumulative 53->52 conversions
    int m_EnforceLastSelectedType = -1;
    uint64 m_EnforceLastSelectedVTable = 0;
};
