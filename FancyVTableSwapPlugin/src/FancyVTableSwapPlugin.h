#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include <memory>
#include <set>

class AvailableParkourAction;

// Passive freeform-wallrun plugin. While the hotkey is ACTIVE:
//   - natural freeform 52s (wallrunUpFromGroundFailed_mb) are kept alive at
//     creation (the vanilla "InitialTestIfActionFits" reject is waived);
//   - wallrun 53 (wallrunUpFromGround_mb) and wallrun-to-overhang 54
//     (wallrunUpToHang) are discarded at creation via the vanilla destroy path.
// Nothing else: no sort&select ownership (FreeJump must keep its own normal
// path - it degrades to its heavy FinalFilter1 fallback cave when we patch
// that site), no conversion, no weight boost, no force-select, no lift.
// The game picks among the surviving candidates itself.
class FancyVTableSwapPlugin
{
public:
    FancyVTableSwapPlugin();
    ~FancyVTableSwapPlugin();

    void OnBeforeActivate();
    void RefreshHookOwnership();
    void OnUpdate();        // per-frame: key + rebind
    void OnImGuiRender();   // ACUPluginLoader menu panel

    // Creation-stage hooks only (see class comment for why).
    bool OnCreationDiscardOverride(AvailableParkourAction& action);
    bool OnForceCreationDiscard(AvailableParkourAction& action);

private:
    bool IsActive() const;
    void LoadSettings();
    void SaveSettings() const;

    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_ActivatorCreation;

    // Config (persisted to INI)
    bool m_Enabled = true;
    int m_Hotkey = 0x77; // F8
    bool m_HoldMode = true;
    bool m_ToggleState = false;
    bool m_WaitingForKey = false;

    // Keys held at the moment "Rebind" was clicked - rebinding accepts only
    // fresh presses (keys NOT in this set), ignoring stale transition bits.
    std::set<int> m_KeysDownAtCapture;
};
