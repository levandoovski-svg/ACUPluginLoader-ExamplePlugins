#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include <array>
#include <memory>

class AvailableParkourAction;

// Gamepad bindable codes, 0x1000+ range (same pattern as
// ExactPositionTeleportPlugin/FreeJumpPlugin: keyboard/mouse live in the Win32
// virtual-key range 0x01..0xFE, pads here). XInput is polled by the plugin.
enum WallrunPadCode
{
    WallrunPad_DPadUp = 0,
    WallrunPad_DPadDown,
    WallrunPad_DPadLeft,
    WallrunPad_DPadRight,
    WallrunPad_Start,
    WallrunPad_Back,
    WallrunPad_L3,
    WallrunPad_R3,
    WallrunPad_LB,
    WallrunPad_RB,
    WallrunPad_A,
    WallrunPad_B,
    WallrunPad_X,
    WallrunPad_Y,
    WallrunPad_LeftTrigger,
    WallrunPad_RightTrigger,
    WallrunPad_LeftStickUp,
    WallrunPad_LeftStickDown,
    WallrunPad_LeftStickLeft,
    WallrunPad_LeftStickRight,
    WallrunPad_RightStickUp,
    WallrunPad_RightStickDown,
    WallrunPad_RightStickLeft,
    WallrunPad_RightStickRight,
    WallrunPad_COUNT
};
constexpr int kWallrunPadKeyBase = 0x1000;

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
    void OnUpdate();        // per-frame: input polling + edges + rebind
    void OnImGuiRender();   // ACUPluginLoader menu panel

    // Creation-stage hooks only (see class comment for why).
    bool OnCreationDiscardOverride(AvailableParkourAction& action);
    bool OnForceCreationDiscard(AvailableParkourAction& action);

private:
    bool IsActive() const;
    bool IsBoundKeyDown() const;   // hold-mode: bound key/pad held right now
    bool IsPadCode(int key) const;
    void UpdatePadStates();
    void CaptureBinding(const std::array<bool, WallrunPadCode_COUNT>& padEdges);
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

    // Rising-edge input tracking (0x01..0xFE keyboard/mouse + pad codes).
    std::array<bool, WallrunPadCode_COUNT> m_PadDown {};
    std::array<bool, 0xFF> m_KbDownPrev {};
    std::array<bool, 0xFF> m_KbPressed {};
    std::array<bool, WallrunPadCode_COUNT> m_PadDownPrev {};
};