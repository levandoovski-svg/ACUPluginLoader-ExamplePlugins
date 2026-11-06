#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include <memory>
#include <vector>
#include <mutex>

class AvailableParkourAction;

// Minimal lazy plugin: swap AvailableParkourAction::fancyVTable live.
// Mechanism from ACUFixes 1.5.0 Request_Spindescent.cpp (climbFacade swap:
// *(uint64*)((uint64)obj+8) = newTable) moved into the official plugin seam
// (GenericHooksInParkourFiltering ChooseBeforeFiltering), so it runs before
// fitness/sort/final-filters and needs no raw patch addresses of its own.
class FancyVTableSwapPlugin
{
public:
    FancyVTableSwapPlugin();
    ~FancyVTableSwapPlugin();

    void OnBeforeActivate();
    void OnUpdate();        // per-frame: key + rebind
    void OnImGuiRender();   // ACUPluginLoader menu panel

    // Called on game thread with the full candidate array for this parkour
    // cycle. Mutates in place; returns nullptr (never force-selects).
    AvailableParkourAction* ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions);

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
    void ApplyHexInputs();

    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_Activator;

    // Config (persisted to INI)
    bool m_Enabled = true;
    int m_Hotkey = 0x77; // F8
    bool m_HoldMode = true;
    bool m_ToggleState = false;
    bool m_WaitingForKey = false;
    uint64 m_SourceVTable = 0; // 0 = match by type only (when type filter set)
    uint64 m_DestVTable = 0;
    int m_TypeFilter = -1;     // -1 = any type; else EnumParkourAction value
    char m_SourceHex[32] = "0x0";
    char m_DestHex[32] = "0x0";
    bool m_HexDirty = false;

    // Runtime snapshot (written on game thread, read by UI)
    // shortcut: single snapshot + mutex, no ring buffer; fine for the
    // ~dozens of actions in one parkour cycle. Upgrade to history only if
    // cycle-to-cycle flicker becomes unreadable.
    mutable std::mutex m_Mutex;
    std::vector<DiscoveredVTable> m_Discovered;
    int m_LastActionsSeen = 0;
    int m_LastSwapped = 0;
};
