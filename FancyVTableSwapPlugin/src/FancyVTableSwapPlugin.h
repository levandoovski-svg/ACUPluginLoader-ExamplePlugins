#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include <memory>
#include <vector>
#include <mutex>

class AvailableParkourAction;

// Single-purpose plugin: while the hotkey is ACTIVE, favor the freeform
// wallrun (wallrunUpFromGroundFailed_mb / type 52) over the normal wallrun
// (type 53) and wallrun-to-overhang — the proven CheatEngine approach: BOOST
// the natural 52's weight, KEEP it alive at creation, and FORCE-SELECT it.
// No vtable swapping: a converted 53 runs with 53-computed trajectory data
// and behaves unpredictably.
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
    // 0 fallback post-construct convert: while armed, convert naturally-created
    //   53 -> 52. The game always creates 53s (also on non-standable objects
    //   where native 52s never appear), so this is the backup path; selection
    //   still prefers natural-born 52s when the game made some itself.
    //   (Skipped in passive mode.)
    void OnPostConstruct(AvailableParkourAction& action);
    // 1 creation-discard override: keep naturally-created 52s alive despite
    // the vanilla "InitialTestIfActionFits" reject. No vtable swapping: the
    // freeform wallrun must use the 52 the game created itself.
    bool OnCreationDiscardOverride(AvailableParkourAction& action);
    // 1b force creation-discard (passive mode only): while armed AND passive,
    // discard every naturally-created 53 upon creation via the vanilla destroy
    // path. The survivors (natural 52s) are then picked by the game itself —
    // no sort&select takeover, so FreeJump keeps its own cheap clear-the-list
    // path instead of its FinalFilter1 fallback cave.
    bool OnForceCreationDiscard(AvailableParkourAction& action);
    // 3 force-select: return the best type-52 in the sorted array (prefer one
    // that passed both final filters; the game's own pick is ignored while
    // armed). Not subscribed in passive mode.
    AvailableParkourAction* ChooseAfterSorting(SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame);

    // Request/release the GPH hooks to match the current mode so FreeJump never
    // sees the sort&select site patched while passive.
    void RefreshHookOwnership();

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
    bool m_PassiveDiscard53 = false; // passive mode: discard 53 at creation, no sort ownership
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
    int m_EnforceConverted = 0;      // cumulative fallback 53->52 conversions
    int m_EnforceKeptAlive = 0;      // cumulative native 52s kept alive
    int m_EnforceLastSelectedType = -1;
    uint64 m_EnforceLastSelectedVTable = 0;
};
