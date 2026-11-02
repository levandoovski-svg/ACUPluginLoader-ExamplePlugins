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
// These are PHYSICAL button names read through the plugin's own independent
// XInput polling, and they are what the parkour feature bindings (Free Jump,
// overrides, dive hotkey, Parkour Mode's own toggle) are assigned to. They are
// deliberately NOT what the Parkour Mode BLOCK LIST stores: a blocked control
// is recorded as the exact control the GAME reports (see kPadSlotBase /
// kPadHatBase below), so the list never has to translate between the plugin's
// view of the pad and the game's.
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

// A blocked entry may also name a raw GAME PAD CONTROL directly, instead of a
// physical button name. This is the ONLY form a blocked controller entry
// takes, and it is what makes the blocked list work with any controller and
// any emulation: the entry is the exact control the GAME reports in its own
// pad state, measured at the moment the player assigns it, so no XInput and
// no assumption about how the pad presents itself is involved.
//   kPadSlotBase + slot    a digital button: the byte at state + 0x30 + slot,
//                          pressed when its bit 7 is set (DirectInput
//                          DIJOYSTATE rgbButtons, as the game reads it).
//   kPadHatBase + degrees  a d-pad direction: the POV dword at state + 0x20
//                          (DirectInput hundredths of a degree, 0xFFFFFFFF =
//                          centered). Only that exact direction is cleared.
// Codes in these ranges are valid blocked-list entries only - they are
// deliberately NOT bindable for the other features (Free Jump, overrides,
// ...), which are bound to physical buttons by the player.
constexpr int kPadSlotBase = 0x2000;
constexpr int kMaxPadSlot = 0xDF;   // last digital-button byte: 0x30 + 0xDF = 0x10F
constexpr int kPadHatBase = 0x2100;   // above the disjoint slot range
constexpr int kMaxPadHatDegrees = 359;
constexpr int kMaxBlockedKey = kPadHatBase + kMaxPadHatDegrees;

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

    // ��� Parkour Mode: capture and block the game's own controls �����������������
    // Binding a controller button and blocking it are the SAME operation, on the
    // same data. The game's pad poll produces one 0x110-byte DirectInput pad
    // state per frame in a buffer both of the game's pad paths consume; the
    // plugin observes that exact buffer and, when the player assigns a button,
    // records the exact control it finds pressed in it (a digital button byte,
    // or a d-pad POV value). Blocking then clears that very control in that very
    // buffer. No button name, no XInput-to-DirectInput translation, no table of
    // assumed slots: whatever the game reports for the button is what gets
    // blocked, so it cannot drift with the emulation (DS4Windows output mode,
    // Steam Input, a raw DualSense) the way a translated mapping does.
    void NoteGamePadState(const uint8_t* padState);
    // The game-side control that went from released to pressed in the currently
    // observed pad state, as a blocked-list code (`kPadSlotBase + slot` for a
    // digital button, `kPadHatBase + degrees` for a d-pad direction), or 0 when
    // there is none. This is the whole "binding" step for a controller.
    int GetRisingGamePadControl() const;
    bool RememberBlockedKey(int blockedKeyCode);
    bool IsBlockedKeyCapturePending() const { return m_WaitingForBlockedKey; }
    bool IsWatchingGamePadInput() const { return m_WatchGamePadInput; }
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
    // included) plus controller entries, which are ALWAYS the game's own
    // identifiers - `kPadSlotBase+slot` for a digital button and
    // `kPadHatBase+degrees` for a d-pad direction, both measured from the pad
    // state the GAME reports at the moment the button is assigned. That is why
    // the list needs no XInput and works identically for a raw DualSense, a
    // DS4Windows pad and a Steam Input pad.
    // Persisted as decimal codes separated by `|` in `ParkourBlockedKeys=`.
    // Defaults are the physical keys the game binds attack/aim/crouch to by
    // default (left and right mouse button + Left Ctrl), so a fresh install
    // behaves like the old pre-selected checkbox set without any setup.
    std::vector<int> m_UserBlockedKeys{ VK_LBUTTON, VK_RBUTTON, VK_LCONTROL };
    bool m_WaitingForBlockedKey = false;   // UI rebind target, false = none
    // Entries from a pre-0.15 INI that this build can no longer honour: the old
    // list stored XInput button NAMES for the controller, which never described
    // the game's own pad state and are therefore not a blockable target. They
    // are dropped at load instead of silently doing nothing; the count is shown
    // once in the UI so the player knows to re-assign those buttons.
    int m_DroppedLegacyPadBindings = 0;
    // Optional diagnostics-only observation of the game's pad input, so the
    // player can see what the game's own device layer reports for a controller
    // even when nothing is blocked. Off by default: the observation seams are
    // hooks, and this codebase only installs them when the feature runs.
    bool m_WatchGamePadInput = false;

    // The game's own pad state, as observed this frame and last frame by the ONE
    // pad seam (the poll's destination buffer). Powers the UI's "what the game
    // sees" readout and the binding capture: a controller button is assigned by
    // finding the control that went from released to pressed between these two
    // snapshots.
    std::array<uint8_t, 0x110> m_GamePadSnapshot{};
    std::array<uint8_t, 0x110> m_GamePadSnapshotPrev{};
    // Set by NoteGamePadState (the pad seam, or the container fallback read),
    // consumed by the binding capture INSIDE OnUpdate, and rolled into
    // ...LastFrame at the END of OnUpdate - AFTER the capture. Resetting this
    // flag at the start of OnUpdate makes the capture dead code: no observation
    // can arrive between the reset and the capture.
    bool m_GamePadObservedThisFrame = false;
    // What OnImGuiRender reads: whether anything was observed in the window
    // between the previous OnUpdate and this one.
    bool m_GamePadObservedLastFrame = false;

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
    // Fallback detach of the game's DirectInput/HID pad while Parkour Mode is
    // on. Off by default: it also disables the sticks, and the pad-poll site
    // below normally frees the assigned buttons one by one. The plugin's own
    // XInput polling always keeps working, so override triggers bound to pad
    // buttons still fire either way.
    bool m_BlockPadWhileParkour = false;
    bool m_PadBlockHookActive = false;
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
    //   0x142728A1F the game's own pad poll, at the exact instruction after
    //                GetDeviceState filled the 0x110-byte DirectInput state in
    //                rsi. THE one controller seam, for both jobs: it observes
    //                that buffer (binding capture + the UI's "game sees"
    //                readout) and clears the assigned controls from it. This
    //                single site covers BOTH of the game's pad paths, because
    //                both callers converge on this poll and this buffer (the
    //                device-id-9 keymap fill writes InputContainerBig+0x864,
    //                the hard-coded per-device mapper reads its own stack copy -
    //                in both cases the very buffer rsi points at), so there is
    //                no second, device-specific hook to keep in sync.
    // The controller-specific extra hooks that used to accompany it (the
    // device-9 snapshot site 0x14273BEB3, the per-device mapper site
    // 0x14273A726 and their action-level suppression) are gone: they existed to
    // translate an assumed button mapping, and with the button cleared at its
    // source there was nothing left for them to do. The old action-mask sites
    // (keyStates at 0x14273BC7B / 0x14273BEC5 / 0x14273A726 and the pad
    // snapshot at 0x14273BEB3) are gone for the same reason.
    bool m_KbmFillSuppressActive = false;
    bool m_PadPollSanitizeActive = false;
    // The input-aggregate seam at 0x142739DBC - the exact site HybridInput.dll
    // detours to merge secondary device states into the container. Runs after
    // every keyStates writer, so suppressed actions are zeroed there one last
    // time (and HybridInput's own merge cannot resurrect them while we own it).
    // Keyboard/mouse only: controller controls never reach this layer, they are
    // already gone at the pad poll.
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
    // the KBM keymap stage, both gamepad keyStates writers, and the optional
    // pad-device detach (see FreeJumpPlugin.cpp).
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
