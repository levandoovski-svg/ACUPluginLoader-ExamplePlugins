#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

class AvailableParkourAction;

// Gamepad bindable codes, 0x1000+ range (same pattern as
// FreeFormWallrun/FreeJump: keyboard/mouse live in the Win32 virtual-key
// range 0x01..0xFE, pads here). XInput is polled by the plugin.
enum DiscardPadCode
{
    DiscardPad_DPadUp = 0,
    DiscardPad_DPadDown,
    DiscardPad_DPadLeft,
    DiscardPad_DPadRight,
    DiscardPad_Start,
    DiscardPad_Back,
    DiscardPad_L3,
    DiscardPad_R3,
    DiscardPad_LB,
    DiscardPad_RB,
    DiscardPad_A,
    DiscardPad_B,
    DiscardPad_X,
    DiscardPad_Y,
    DiscardPad_LeftTrigger,
    DiscardPad_RightTrigger,
    DiscardPad_LeftStickUp,
    DiscardPad_LeftStickDown,
    DiscardPad_LeftStickLeft,
    DiscardPad_LeftStickRight,
    DiscardPad_RightStickUp,
    DiscardPad_RightStickDown,
    DiscardPad_RightStickLeft,
    DiscardPad_RightStickRight,
    DiscardPad_COUNT
};
constexpr int kDiscardPadKeyBase = 0x1000;

// Lets the user pick arbitrary parkour moves (EnumParkourAction values) which
// are discarded at creation through the vanilla destroy path - the same
// mechanism FreeFormWallrun uses to kill wallruns 53/54. Discarding happens
// ONLY while the hotkey mechanism is ACTIVE (hold/toggle), so the choice is
// momentary and safe to hold over normal gameplay.
class DiscardMovePlugin
{
public:
    DiscardMovePlugin();
    ~DiscardMovePlugin();

    void OnBeforeActivate();
    void RefreshHookOwnership();
    void OnUpdate();        // per-frame: input polling + edges + rebind
    void OnImGuiRender();   // ACUPluginLoader menu panel

    // Only the force-discard hook is used: return true to destroy the action.
    bool OnForceCreationDiscard(AvailableParkourAction& action);
    // Keep-alive: waive the vanilla reject for the enforced move so the
    // enforce pick below always has a surviving candidate.
    bool OnCreationDiscardOverride(AvailableParkourAction& action);
    AvailableParkourAction* OnChooseAfterSorting(
        SmallArray<AvailableParkourAction*>& allActionsSorted,
        AvailableParkourAction* selectedByGame);

private:
    bool IsActive() const;          // discard gating (m_Hotkey, hold/toggle)
    bool IsEnforceActive() const;   // enforce gating (m_EnforceHotkey, hold)
    bool IsBoundKeyDown() const;   // hold-mode: bound key/pad held right now
    bool IsPadCode(int key) const;
    void UpdatePadStates();
    void CaptureBinding(int* targetKey, const std::array<bool, DiscardPad_COUNT>& padEdges);
    void LoadSettings();
    void SaveSettings() const;
    bool IsDiscarded(int actionType) const;

    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_ActivatorCreation;
    // Owned only while the enforce key is held (dynamic, see OnUpdate):
    // avoids permanently patching the sort&select site next to FreeJump
    // (its FinalFilter1 fallback cave) and the ACU parkour debugger.
    std::shared_ptr<SharedHookActivator> m_ActivatorSortAndSelect;

    // Diagnostics ring: actions seen at the creation hook (any thread).
    // type -2 = slot not yet written, -1 = type unreadable.
    static constexpr int kDiagCount = 12;
    struct DiagEntry { int type = -2; bool wasOnList = false; bool discardApplied = false; };
    DiagEntry m_Diag[kDiagCount]{};
    std::atomic<int> m_DiagWriteIdx{ 0 };
    bool m_ShowDiagnostics = false;

    // Config (persisted to INI)
    bool m_Enabled = true;
    int m_Hotkey = 0x77; // F8 - discard key
    int m_EnforceHotkey = 0x76; // F7 - enforce jumpIntoWindow while held
    bool m_HoldMode = true;
    bool m_ToggleState = false;
    bool m_WaitingForKey = false;
    bool m_EnforceWaitingForKey = false;

    // The moves to discard, persisted to INI as enum names.
    std::vector<EnumParkourAction> m_DiscardList;
    // ImGui picker state.
    int m_PendingPickIdx = 0;

    // Rising-edge input tracking (0x01..0xFE keyboard/mouse + pad codes).
    std::array<bool, DiscardPad_COUNT> m_PadDown {};
    std::array<bool, 0xFF> m_KbDownPrev {};
    std::array<bool, 0xFF> m_KbPressed {};
    std::array<bool, DiscardPad_COUNT> m_PadDownPrev {};
};
