#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"

class AvailableParkourAction;
enum class EnumParkourAction : int;

// Bindable input codes..
// Keyboard/mouse codes are Win32 virtual-key codes (0x01..0xFE), which is what
// the existing FreeJumpPlugin.ini files already store. Gamepad codes live in a
// separate range above 0xFF so old INI values keep their meaning.
constexpr int kKeyboardKeyMin = 0x01;
constexpr int kKeyboardKeyMax = 0xFE;

// One entry per XInput button pair / digital stick direction / digital trigger.
// Codes are persisted as decimal integers (like override keyCodes always were).
// These are the SAME codes the whole plugin uses: feature bindings (Free Jump,
// overrides, dive hotkey, Parkour Mode's own toggle) AND the Parkour Mode BLOCK
// LIST (see the controller hook in FreeJumpPlugin.cpp: the game reads pads
// through XInputGetState, so blocking one of these clears that button in the
// state the game is about to receive). One capture path for every device.
enum ParkourPadCode
{
    PadCode_DPAD_UP = 0,
    PadCode_DPAD_DOWN,
    PadCode_DPAD_LEFT,
    PadCode_DPAD_RIGHT,
    PadCode_START,
    PadCode_BACK,
    PadCode_L3,
    PadCode_R3,
    PadCode_LB,
    PadCode_RB,
    PadCode_A,
    PadCode_B,
    PadCode_X,
    PadCode_Y,
    PadCode_LT,
    PadCode_RT,
    PadCode_LSTICK_UP,
    PadCode_LSTICK_DOWN,
    PadCode_LSTICK_LEFT,
    PadCode_LSTICK_RIGHT,
    PadCode_RSTICK_UP,
    PadCode_RSTICK_DOWN,
    PadCode_RSTICK_LEFT,
    PadCode_RSTICK_RIGHT,
    PadCode_COUNT
};
constexpr int kPadKeyBase = 0x1000;
constexpr int kMaxBindableKey = kPadKeyBase + PadCode_COUNT;

// The game maps raw keyboard/mouse input to 32 ActionKeyCode entries per frame
// (ActionKeyCode::ACTION_SET_SIZE == 0x20 in Enum_ActionKeyCode.h).
constexpr int kActionKeyCount = 0x20;

struct ParkourActionOverride
{
    std::string name;                // user-visible label, defaults per index
    int keyCode = VK_F9;             // keyboard/mouse-profile binding
    int padKeyCode = 0;              // controller-profile binding (0 = unbound)
    std::vector<int> actionTypes = { 50 };
    float fitnessMultiplier = 9999.0f;
    bool removeOtherActions = true;
    bool enable = true;
    bool toggleMode = false;
    bool toggleState = false;
    bool expandRanges = false;       // FailedWallRun-style only-loosen acceptance-range widening while active
    bool waitingForKey = false;
};

class FreeJumpPlugin
{
public:
    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();

    AvailableParkourAction* ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions);
    AvailableParkourAction* ChooseAfterSorting(SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame);

    // Read by the wall-to-hang hooks: true while the feature is enabled and
    // the dive hotkey is held (and that hold is not a UI click or a fresh
    // rebind capture).
    bool ShouldForceWallToHang() const;

    // Parkour Mode no longer has per-action checkboxes. The player lists the
    // physical buttons (keyboard keys, mouse buttons, controller buttons) the
    // game must not see while the mode is ON; those same buttons stay readable
    // by the plugin itself, so they can drive override/parkour bindings at the
    // same time. See `DescribeBlockedInputTarget` in FreeJumpPlugin.cpp for what
    // each entry clears in the game's own raw input buffers.
    const std::vector<int>& GetBlockedKeys() const { return m_UserBlockedKeys; }
    // Human-readable one-liner for a blocked entry, so the UI can state exactly
    // what the game will stop seeing.
    static std::string DescribeBlockedInputTarget(int bindableKeyCode);

    // Binding a controller button and blocking it use the SAME codes: a
    // blocked controller entry is a PadCode_*, captured with the same
    // captureKey lambda as every other binding, and cleared from the XInput
    // state the game is about to receive (see the controller hook in
    // FreeJumpPlugin.cpp).
    bool RememberBlockedKey(int blockedKeyCode);
    bool IsBlockedKeyCapturePending() const { return m_WaitingForBlockedKey; }
private:
    void UpdateKeyState(int bindableKeyCode);
    void UpdateAllKeyStates();
    bool WasKeyPressed(int bindableKeyCode) const;
    bool IsKeyHeld(int bindableKeyCode) const;
    void UpdatePadStates();
    bool IsSortAndSelectSiteAlreadyPatched() const;

    void LoadSettings();
    void SaveSettings();

    // GenericHooksInParkourFiltering callback system
    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_HookActivator;
    bool m_HookActive = false;
    bool m_HookRefused = false;

    // Basic features. Every binding exists twice — a keyboard/mouse slot and a
    // controller slot — so the profile switch swaps between saved bindings
    // without rebinding. The controller slot may be any bindable code
    // (keyboard codes remain functional there too).
    int  m_ToggleKey         = VK_XBUTTON1;
    int  m_ToggleKeyPad      = VK_XBUTTON1;
    bool m_WaitingForKey     = false;
    bool m_UseControllerBindings = false;

    // Parkour Mode: while active, the selected ActionKeyCode entries are forced
    // unpressed so their physical keys are free to trigger override bindings.
    int  m_ParkourModeKey    = VK_F7;
    int  m_ParkourModeKeyPad = VK_F7;
    bool m_ParkourModeOn     = false;
    bool m_WaitingForParkourKey = false;

    // Parkour Mode: the physical buttons the game must not see while the mode is
    // ON. Keyboard/mouse Win32 virtual keys (0x01..0xFE, mouse buttons
    // included) plus the same PadCode_* controller codes every other feature
    // uses (see the controller hook in FreeJumpPlugin.cpp). One capture path
    // for every device.
    // Persisted as decimal codes separated by `|` in `ParkourBlockedKeys=`.
    // Defaults are the physical keys the game binds attack/aim/crouch to by
    // default (left and right mouse button + Left Ctrl), so a fresh install
    // behaves like the old pre-selected checkbox set without any setup.
    std::vector<int> m_UserBlockedKeys{ VK_LBUTTON, VK_RBUTTON, VK_LCONTROL };
    bool m_WaitingForBlockedKey = false;   // UI rebind target, false = none

    // Wall-to-Hang (ported from ACUFixes-0.9.3 Parkour Helpers / dive helper).
    // While the dive hotkey is held, wall-eject-to-hang candidates are created
    // by hooking the ClimbFacade/BackEject scanners, the climbFacade tester's
    // fancy vtable is swapped to the sidehop-to-swing variant, the eject-to-hang
    // acceptance ranges are widened, the backeject RTCP angle is fixed, and the
    // dive/toHang move takes selection priority.
    bool m_WallToHangEnabled = true;
    int  m_DiveHotkey        = VK_XBUTTON2;
    int  m_DiveHotkeyPad     = 0;
    bool m_WaitingForDiveKey = false;
    bool m_WallToHangHooksActive = false;
    bool m_WallToHangSiteTaken   = false;   // a site is E9-patched by another plugin
    bool m_WallToHangGphRefused  = false;   // sort&select owned by another plugin
    // Parkour-mode suppression sites, installed independently of each other. Each
    // one sits at the single point where the game's own copy of a device's state
    // is created, so a blocked control is erased before ANY consumer can read
    // it - no action, no press edge, no hold, no analog mirror:
    //   0x14273B2B0 the keyboard/mouse keymap fill, hooked at its entry. Called
    //                with r8 = this frame's keyboard scancode array and rdx =
    //                the mouse state (DIMOUSESTATE, buttons at +0xC); clearing
    //                a blocked key there means the fill can never turn it into
    //                an ActionKeyCode, an analog mirror (+0x274/+0x29C/+0x2A8)
    //                or a press edge.
    //   controller  the game's XInputGetState import (xinput1_3.dll), detoured
    //                so the state the game is about to receive has the blocked
    //                buttons cleared (see FreeJumpPlugin.cpp).
    bool m_KbmFillSuppressActive = false;
    // The input-aggregate seam at 0x142739DBC - the exact site HybridInput.dll
    // detours to merge secondary device states into the container. Runs after
    // every keyStates writer, so suppressed actions are zeroed there one last
    // time (and HybridInput's own merge cannot resurrect them while we own it).
    // Keyboard/mouse only: controller buttons never reach this layer, they are
    // already gone from the XInput state the game reads.
    bool m_InputMergeSeamActive = false;
    bool m_ParkourHookRefused = false; // at least one site owned by another plugin
    bool m_OverrideFallbackActive = false;
    bool m_OverrideFallbackRefused = false;

    // Reliable Win32 rising-edge state. The low bit of GetAsyncKeyState is
    // only "pressed since the previous call" and is not reliable for this.
    // The arrays cover keyboard/mouse codes plus the gamepad code range.
    std::array<bool, kMaxBindableKey + 1> m_PreviousKeyDown{};
    std::array<bool, kMaxBindableKey + 1> m_KeyPressedThisFrame{};
    std::array<bool, kMaxBindableKey + 1> m_KeyStateUpdatedThisFrame{};
    int m_JustCapturedKey = 0;

    // Rebind-capture guard for mouse-button bindings: a rebind is started by a
    // UI click, and that click must not be captured as the new binding. While
    // a rebind is pending, LMB/RMB are only accepted after both were seen
    // released once (FreeJumpPlugin.cpp OnUpdate). Keyboard/pad captures are
    // unaffected.
    bool m_AnyRebindWaitingLastFrame = false;
    bool m_MouseButtonsSettledSinceRebindStart = true;

    // Gamepad (XInput) state. m_PadDown* are indexed by ParkourPadCode.
    std::array<bool, PadCode_COUNT> m_PadDown{};
    std::array<bool, PadCode_COUNT> m_PadPreviousDown{};
    int m_ActivePadSlot = -1;
    bool m_PadDllLoaded = false;

    // Overrides
    std::vector<ParkourActionOverride> m_Overrides;

    // Parkour mode suppression hooks (active only while the mode is enabled):
    // the KBM keymap stage and the controller's XInputGetState detour (see
    // FreeJumpPlugin.cpp).
    void ApplyParkourMode(bool enable);
    // Wall-to-Hang site hooks (see FreeJumpPlugin.cpp).
    void ApplyWallToHang(bool enable);
    // Override fallback used while another plugin owns the sort&select site.
    void ApplyOverrideFallback(bool enable);

public:
    // Fallback path used when another plugin owns the sort&select site, so the
    // normal-path overrides still apply. Called from the FinalFilter1 cave.
    bool ShouldFallbackBlockCandidate(AvailableParkourAction* action);

    // Binding-profile accessors. Both profiles' key states are refreshed every
    // frame, so switching profiles never leaves a stale rising edge behind.
    // Every accessor falls back to the keyboard/mouse slot when the controller
    // slot is still unbound (stored 0 or otherwise invalid) — otherwise a
    // fresh switch to controller bindings silently killed the feature (the
    // dive-hotkey default is 0, so wall-to-hang never fired on controller).
    static bool ValidBindCode(int code)
    {
        return (code >= kKeyboardKeyMin && code <= kKeyboardKeyMax)
            || (code >= kPadKeyBase && code < kMaxBindableKey);
    }
    int& ActiveToggleKey() { return m_UseControllerBindings && ValidBindCode(m_ToggleKeyPad) ? m_ToggleKeyPad : m_ToggleKey; }
    int& ActiveParkourModeKey() { return m_UseControllerBindings && ValidBindCode(m_ParkourModeKeyPad) ? m_ParkourModeKeyPad : m_ParkourModeKey; }
    int EffectiveToggleKey() const { return m_UseControllerBindings && ValidBindCode(m_ToggleKeyPad) ? m_ToggleKeyPad : m_ToggleKey; }
    int EffectiveParkourModeKey() const { return m_UseControllerBindings && ValidBindCode(m_ParkourModeKeyPad) ? m_ParkourModeKeyPad : m_ParkourModeKey; }
    int EffectiveDiveHotkey() const { return m_UseControllerBindings && ValidBindCode(m_DiveHotkeyPad) ? m_DiveHotkeyPad : m_DiveHotkey; }
    int EffectiveOverrideKey(const ParkourActionOverride& override) const
    {
        return m_UseControllerBindings && ValidBindCode(override.padKeyCode) ? override.padKeyCode : override.keyCode;
    }
};
