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

// Bindable input codes.
// Keyboard/mouse codes are Win32 virtual-key codes (0x01..0xFE), which is what
// the existing FreeJumpPlugin.ini files already store. Gamepad codes live in a
// separate range above 0xFF so old INI values keep their meaning.
constexpr int kKeyboardKeyMin = 0x01;
constexpr int kKeyboardKeyMax = 0xFE;

// One entry per XInput button pair / digital stick direction / digital trigger.
// Codes are persisted as decimal integers (like override keyCodes always were).
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

    // Read by the parkour-mode suppression callback (FreeJumpPlugin.cpp).
    uint32_t GetDisabledActionMask() const { return m_DisabledActionMask; }
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
    int  m_SnapDirectionKey  = VK_F8;
    int  m_SnapDirectionKeyPad = VK_F8;
    bool m_WaitingForSnapKey = false;
    bool m_SnapDirectionMode = false;
    bool m_UseControllerBindings = false;

    // Parkour mode: while active, the selected ActionKeyCode entries are forced
    // unpressed so their physical keys are free to trigger override bindings.
    int  m_ParkourModeKey    = VK_F7;
    int  m_ParkourModeKeyPad = VK_F7;
    bool m_ParkourModeOn     = false;
    bool m_WaitingForParkourKey = false;
    // Fallback detach of the game's DirectInput/HID pad while Parkour Mode is
    // on. Off by default: it also disables the sticks, and the per-site
    // suppression below already frees pad buttons one by one. The plugin's own
    // XInput polling always keeps working, so override triggers bound to pad
    // buttons still fire either way.
    bool m_BlockPadWhileParkour = false;
    bool m_PadBlockHookActive = false;
    uint32_t m_DisabledActionMask = (1u << 1) | (1u << 11) | (1u << 10); // LMB, RMB, Crouch
    // Parkour-mode suppression sites, installed independently of each other:
    //   0x14273BC7B keyboard/mouse
    //   0x14273BEC5 gamepad, device-id-9 keymap fill
    //   0x14273A726 gamepad, per-device-type mapper
    bool m_KbmSuppressActive = false;
    bool m_PadKeymapSuppressActive = false;
    bool m_MapperSuppressActive = false;
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
    // the KBM keymap stage, both gamepad keyStates writers, and the optional
    // pad-device detach (see FreeJumpPlugin.cpp).
    void ApplyParkourMode(bool enable);
    // Override fallback used while another plugin owns the sort&select site.
    void ApplyOverrideFallback(bool enable);

public:
    // Fallback path used when another plugin owns the sort&select site, so the
    // normal-path overrides still apply. Called from the FinalFilter1 cave.
    bool ShouldFallbackBlockCandidate(AvailableParkourAction* action);

    // Binding-profile accessors. Both profiles' key states are refreshed every
    // frame, so switching profiles never leaves a stale rising edge behind.
    int& ActiveToggleKey() { return m_UseControllerBindings ? m_ToggleKeyPad : m_ToggleKey; }
    int& ActiveSnapDirectionKey() { return m_UseControllerBindings ? m_SnapDirectionKeyPad : m_SnapDirectionKey; }
    int& ActiveParkourModeKey() { return m_UseControllerBindings ? m_ParkourModeKeyPad : m_ParkourModeKey; }
    int EffectiveToggleKey() const { return m_UseControllerBindings ? m_ToggleKeyPad : m_ToggleKey; }
    int EffectiveSnapDirectionKey() const { return m_UseControllerBindings ? m_SnapDirectionKeyPad : m_SnapDirectionKey; }
    int EffectiveParkourModeKey() const { return m_UseControllerBindings ? m_ParkourModeKeyPad : m_ParkourModeKey; }
    int EffectiveOverrideKey(const ParkourActionOverride& override) const
    {
        return m_UseControllerBindings ? override.padKeyCode : override.keyCode;
    }
};
