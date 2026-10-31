#include "pch.h"
#include "FreeJumpPlugin.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/Parkour_WhileOnWallSystem.h"
#include "ACU/Memory/ACUAllocs.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ACU_DefineNativeFunction.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include <Windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

// NOTE: AvailableParkourAction::GetEnumParkourAction() is defined exactly once,.
// in GenericHooksInParkourFiltering.cpp (compiled in this project). Do NOT
// redefine it here — duplicate definitions break the link.

// ACU 1.5.0 / ACUFixes-0.9.3 verified SmallArray POD-removal entry point.
DEFINE_GAME_FUNCTION(SmallArray_POD__RemoveGeneric, 0x142725F00, void, __fastcall, (void*, int, unsigned int));


namespace
{
    constexpr int kMaxOverrideCount = 4096;

    std::string_view Trim(std::string_view text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        return text;
    }

    bool TryGetValue(std::string_view line, std::string_view prefix, std::string_view& value)
    {
        if (line.rfind(prefix, 0) != 0)
            return false;
        value = Trim(line.substr(prefix.size()));
        return true;
    }

    bool TryParseInteger(std::string_view text, int base, int& value)
    {
        text = Trim(text);
        if (text.empty())
            return false;

        try
        {
            size_t parsedCharacters = 0;
            const long long parsed = std::stoll(std::string(text), &parsedCharacters, base);
            if (parsedCharacters != text.size()
                || parsed < std::numeric_limits<int>::min()
                || parsed > std::numeric_limits<int>::max())
            {
                return false;
            }
            value = static_cast<int>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TryParseFloat(std::string_view text, float& value)
    {
        text = Trim(text);
        if (text.empty())
            return false;

        try
        {
            size_t parsedCharacters = 0;
            const float parsed = std::stof(std::string(text), &parsedCharacters);
            if (parsedCharacters != text.size() || !std::isfinite(parsed))
                return false;
            value = parsed;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TryParseBool(std::string_view text, bool& value)
    {
        text = Trim(text);
        if (text == "0")
        {
            value = false;
            return true;
        }
        if (text == "1")
        {
            value = true;
            return true;
        }
        return false;
    }

    bool IsValidBindableKey(int bindableKeyCode)
    {
        if (bindableKeyCode >= kKeyboardKeyMin && bindableKeyCode <= kKeyboardKeyMax)
            return true;
        return bindableKeyCode >= kPadKeyBase && bindableKeyCode < kPadKeyBase + PadCode_COUNT;
    }

    bool IsPadCode(int bindableKeyCode)
    {
        return bindableKeyCode >= kPadKeyBase && bindableKeyCode < kPadKeyBase + PadCode_COUNT;
    }

    // XInput is loaded dynamically: the loader environment may predate
    // xinput1_4.dll, and no XInput import library is linked into the plugin.
    // The game itself imports xinput1_3.dll, so an XInput-capable pad is already
    // a supported controller for ACU on PC (DualSense via Steam Input or
    // DS4Windows, which present as XInput devices).
    struct XInputPadState
    {
        uint32_t packetNumber;
        uint16_t buttons;
        uint8_t leftTrigger;
        uint8_t rightTrigger;
        int16_t thumbLX, thumbLY, thumbRX, thumbRY;
    };
    using XInputGetState_t = DWORD(WINAPI*)(DWORD, XInputPadState*);

    XInputGetState_t GetXInputGetState()
    {
        static const XInputGetState_t cached = []() -> XInputGetState_t
        {
            const char* dllNames[] = {
                "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll"
            };
            for (const char* dllName : dllNames)
            {
                if (HMODULE module = LoadLibraryA(dllName))
                {
                    if (auto named = (XInputGetState_t)GetProcAddress(module, "XInputGetState"))
                        return named;
                    // xinput9_1_0.dll only exports the function by ordinal.
                    if (auto byOrdinal = (XInputGetState_t)GetProcAddress(module, (const char*)100))
                        return byOrdinal;
                }
            }
            return nullptr;
        }();
        return cached;
    }

    // ── Parkour Mode: how a blocked button maps onto the game's raw input ──
    // Parkour Mode blocks *buttons*, not in-game actions, so a blocked entry is
    // described as "which byte of the game's own this-frame input buffer is
    // cleared". Both blockers act the moment their buffer has just been filled
    // and before any consumer reads it, so nothing downstream (the keymap fill,
    // the pad keymap/mapper, the bool→float mirrors, edge and hold detection)
    // can observe the button at all.
    enum class BlockedInputKind
    {
        KeyboardScancode,   // DirectInput keyboard state byte, bit 7 = pressed
        MouseButton,        // DIMOUSESTATE rgbButtons[i] at mouseState + 0xC
        PadSnapshotButton,  // pad snapshot byte at +0x30 + slot
        PadSnapshotHat,     // pad snapshot POV dword at +0x20 (d-pad)
        Unsupported,        // no known slot in the game's buffers; nothing cleared
    };
    struct BlockedInputSlot
    {
        BlockedInputKind kind = BlockedInputKind::Unsupported;
        int slot = -1;   // byte index inside the buffer named by `kind`
    };

    // Keyboard: the keymap fill indexes the DirectInput keyboard state (the
    // 256-byte buffer the keyboard poll writes) by scancode, with the usual
    // left/right-agnostic special cases (0x1D/0x9D ctrl, 0x2A/0x36 shift,
    // 0x38/0xB8 alt, 0x0F tab). MapVirtualKeyA produces exactly those codes, so
    // blocking "Left Ctrl" clears 0x1D and leaves "Right Ctrl" (0x9D) live —
    // the player blocks precisely the button he assigned, nothing wider.
    BlockedInputSlot ComputeKeyboardSlot(int bindableKeyCode)
    {
        const UINT scancode = MapVirtualKeyA((UINT)bindableKeyCode, MAPVK_VK_TO_VSC);
        if (scancode == 0 || scancode > 0xFF)
            return {};
        return { BlockedInputKind::KeyboardScancode, static_cast<int>(scancode) };
    }

    // Mouse: the keymap fill reads `mouseState[0xC + buttonIndex]` for a type-2
    // (mouse) entry, i.e. DirectInput's DIMOUSESTATE rgbButtons: 0 = left,
    // 1 = right, 2 = middle, 3 = X1, 4 = X2 (the poll asks for the 0x14-byte
    // variant, which has room for 8).
    BlockedInputSlot ComputeMouseButtonSlot(int bindableKeyCode)
    {
        switch (bindableKeyCode)
        {
        case VK_LBUTTON:  return { BlockedInputKind::MouseButton, 0 };
        case VK_RBUTTON:  return { BlockedInputKind::MouseButton, 1 };
        case VK_MBUTTON:  return { BlockedInputKind::MouseButton, 2 };
        case VK_XBUTTON1: return { BlockedInputKind::MouseButton, 3 };
        case VK_XBUTTON2: return { BlockedInputKind::MouseButton, 4 };
        default: break;
        }
        return {};
    }

    // Controller: the plugin's ParkourPadCode -> the game's pad snapshot slot.
    // The digital buttons follow the game's own device-6 (Xbox-style) mapper
    // table, dump-proven from ACU 1.5.0: slot 0 = A, 1 = B, 2 = X, 3 = Y,
    // 4 = RB, 5 = LB, 6 = Start, 7 = Back; L3/R3 extend the same table.
    // The d-pad is not a button there: it is the hat dword at +0x20 that the
    // mapper compares against 0/9000/18000/27000, and 0xFFFFFFFF is
    // DirectInput's "centered" value, which every direction test rejects.
    BlockedInputSlot ComputePadSnapshotSlot(int bindableKeyCode)
    {
        using K = BlockedInputKind;
        if (!IsPadCode(bindableKeyCode))
            return {};
        switch (bindableKeyCode - kPadKeyBase)
        {
        case PadCode_A:     return { K::PadSnapshotButton, 0 };
        case PadCode_B:     return { K::PadSnapshotButton, 1 };
        case PadCode_X:     return { K::PadSnapshotButton, 2 };
        case PadCode_Y:     return { K::PadSnapshotButton, 3 };
        case PadCode_RB:    return { K::PadSnapshotButton, 4 };
        case PadCode_LB:    return { K::PadSnapshotButton, 5 };
        case PadCode_START: return { K::PadSnapshotButton, 6 };
        case PadCode_BACK:  return { K::PadSnapshotButton, 7 };
        case PadCode_L3:    return { K::PadSnapshotButton, 8 };
        case PadCode_R3:    return { K::PadSnapshotButton, 9 };
        case PadCode_DPAD_UP:
        case PadCode_DPAD_DOWN:
        case PadCode_DPAD_LEFT:
        case PadCode_DPAD_RIGHT:
            return { K::PadSnapshotHat, 0 };
        default:
            break;
        }
        // Triggers and stick directions: the game derives those actions from
        // the snapshot's analog axis dwords, which this build has not mapped.
        // Clearing an unknown byte would risk hiding the wrong button, so the
        // entry is reported and skipped instead of being guessed at.
        return {};
    }

    const char* PadCodeName(int bindableKeyCode)
    {
        static const char* names[PadCode_COUNT] = {
            "Pad DPad Up", "Pad DPad Down", "Pad DPad Left", "Pad DPad Right",
            "Pad Start", "Pad Back",
            "Pad L3", "Pad R3",
            "Pad LB", "Pad RB",
            "Pad A", "Pad B", "Pad X", "Pad Y",
            "Pad Left Trigger", "Pad Right Trigger",
            "Pad LStick Up", "Pad LStick Down", "Pad LStick Left", "Pad LStick Right",
            "Pad RStick Up", "Pad RStick Down", "Pad RStick Left", "Pad RStick Right",
        };
        if (!IsPadCode(bindableKeyCode))
            return "";
        return names[bindableKeyCode - kPadKeyBase];
    }

    std::string KeyCodeLabel(int bindableKeyCode)
    {
        if (IsPadCode(bindableKeyCode))
        {
            std::ostringstream label;
            label << "0x" << std::uppercase << std::hex << bindableKeyCode
                  << " (" << PadCodeName(bindableKeyCode) << ")";
            return label.str();
        }
        const char* mouseName = nullptr;
        switch (bindableKeyCode)
        {
        case VK_LBUTTON:  mouseName = "Mouse Left"; break;
        case VK_RBUTTON:  mouseName = "Mouse Right"; break;
        case VK_MBUTTON:  mouseName = "Mouse Middle"; break;
        case VK_XBUTTON1: mouseName = "Mouse X1"; break;
        case VK_XBUTTON2: mouseName = "Mouse X2"; break;
        default: break;
        }
        std::ostringstream label;
        label << "0x" << std::uppercase << std::hex << bindableKeyCode;
        if (mouseName)
            label << " (" << mouseName << ")";
        return label.str();
    }

    // LMB/RMB (and the other mouse buttons) are bindable. While the loader
    // ImGui menu is open and the cursor is over ImGui, mouse-button input
    // belongs to the UI, so a mouse-button binding must neither toggle a mode
    // nor trigger a hold from menu clicks. Keyboard/pad codes are never
    // blocked by this.
    bool MouseBindWouldHitUi(int bindableKeyCode)
    {
        switch (bindableKeyCode)
        {
        case VK_LBUTTON:
        case VK_RBUTTON:
        case VK_MBUTTON:
        case VK_XBUTTON1:
        case VK_XBUTTON2:
            break;
        default:
            return false;
        }
        if (!ImGui::GetCurrentContext())
            return false;
        return ImGui::GetIO().WantCaptureMouse;
    }

    std::vector<std::string_view> Split(std::string_view text, char delimiter)
    {
        std::vector<std::string_view> parts;
        size_t partStart = 0;
        while (true)
        {
            const size_t delimiterPosition = text.find(delimiter, partStart);
            if (delimiterPosition == std::string_view::npos)
            {
                parts.push_back(text.substr(partStart));
                break;
            }
            parts.push_back(text.substr(partStart, delimiterPosition - partStart));
            partStart = delimiterPosition + 1;
        }
        return parts;
    }

    bool TryParseOverride(std::string_view data, int& index, ParkourActionOverride& parsedOverride)
    {
        // 8 mandatory fields + optional 9th (pad key) and 10th (expand ranges),
        // so pre-existing 8-field INI lines keep loading.
        const std::vector<std::string_view> fields = Split(data, ',');
        if (fields.size() != 8 && fields.size() != 10 && fields.size() != 11)
            return false;

        int overrideIndex = 0;
        int keyCode = 0;
        if (!TryParseInteger(fields[0], 10, overrideIndex)
            || !TryParseInteger(fields[1], 10, keyCode)
            || !IsValidBindableKey(keyCode))
        {
            return false;
        }

        std::vector<int> actionTypes;
        for (const std::string_view actionTypeText : Split(fields[2], '|'))
        {
            if (actionTypeText.empty())
                continue;
            int actionType = 0;
            if (!TryParseInteger(actionTypeText, 10, actionType) || actionType < 0)
                return false;
            actionTypes.push_back(actionType);
        }
        if (actionTypes.empty())
            actionTypes.push_back(50); // Legacy files used the default type for an empty field.

        float fitnessMultiplier = 0.0f;
        bool removeOtherActions = false;
        bool enabled = false;
        bool toggleMode = false;
        bool toggleState = false;
        if (!TryParseFloat(fields[3], fitnessMultiplier)
            || !TryParseBool(fields[4], removeOtherActions)
            || !TryParseBool(fields[5], enabled)
            || !TryParseBool(fields[6], toggleMode)
            || !TryParseBool(fields[7], toggleState))
        {
            return false;
        }

        // Newer files carry the controller-profile key and the range-widening
        // flag. Older 8-field lines keep their keyboard key in both slots.
        int padKeyCode = keyCode;
        bool expandRanges = false;
        if (fields.size() >= 10)
        {
            if (!TryParseInteger(fields[8], 10, padKeyCode) || !IsValidBindableKey(padKeyCode)
                || !TryParseBool(fields[9], expandRanges))
            {
                return false;
            }
        }

        index = overrideIndex;
        parsedOverride.keyCode = keyCode;
        parsedOverride.padKeyCode = padKeyCode;
        parsedOverride.actionTypes = std::move(actionTypes);
        parsedOverride.fitnessMultiplier = fitnessMultiplier;
        parsedOverride.removeOtherActions = removeOtherActions;
        parsedOverride.enable = enabled;
        parsedOverride.toggleMode = toggleMode;
        parsedOverride.toggleState = toggleState;
        parsedOverride.expandRanges = expandRanges;
        // Commas are the field separator, so a stored name can never contain
        // one; the UI editor replaces them on edit too.
        if (fields.size() == 11)
        {
            std::string parsedName(fields[10]);
            std::replace(parsedName.begin(), parsedName.end(), ',', ';');
            parsedOverride.name = std::move(parsedName);
        }
        parsedOverride.waitingForKey = false;
        return true;
    }

    // Some transient candidates have a NULL GetEnumParkourAction fancy slot.
    // Calling the member method on those candidates dispatches to address zero
    // and crashes, so validate the slot before reading an action type.
    int GetTypeIntSafe(const AvailableParkourAction* action)
    {
        if (!action || !action->fancyVTable)
            return -1;

        const FancyVFunction& slot =
            action->fancyVTable->fancyVirtuals[ParkourActionKnownFancyVFuncs::GetEnumParkourAction::idx];
        if (!slot.fn)
            return -1;

        return static_cast<int>(
            GET_AND_CAST_FANCY_FUNC(
                *action,
                ParkourActionKnownFancyVFuncs::GetEnumParkourAction)(
                    const_cast<AvailableParkourAction*>(action)));
    }

    // Called from the fallback cave for every parkour candidate while our own
    // ChooseBeforeFiltering is unavailable (another plugin owns sort&select).
    // Returns 1 to force the candidate out of the selection, 0 to leave the
    // game's verdict untouched. Mirrors the semantics of
    // FreeJumpPlugin::ChooseBeforeFiltering, minus the fitness multiplier.
    FreeJumpPlugin* g_OverrideFallbackInstance = nullptr;

    int __fastcall ParkourOverrideFallbackDecide_Impl(AvailableParkourAction* action)
    {
        FreeJumpPlugin* owner = g_OverrideFallbackInstance;
        if (!owner || !action)
            return 0;
        return owner->ShouldFallbackBlockCandidate(action) ? 1 : 0;
    }

    // ── Parkour mode: block the player's assigned buttons ─────────────
    //
    // Parkour Mode no longer ticks in-game actions off a list. The player
    // assigns BUTTONS (keyboard keys, mouse buttons, controller buttons), and
    // each assigned button is erased from the game's own copy of this frame's
    // raw input before anything reads it. That is what makes the assignment
    // reliable: an in-game binding to a blocked key cannot fire regardless of
    // which ActionKeyCode it drives, whether the player rebound it, or whether
    // the game reaches that key through its keyboard, mouse or controller
    // bindings — and the plugin's own polling (Win32 GetAsyncKeyState and its
    // independent XInput reader) is untouched, so the same button can drive a
    // parkour trigger at the very same moment.
    //
    // Two seams, each one at the single point where the game's copy of a
    // device's state is created:
    //
    //   keyboard/mouse — the keymap fill at 0x14273B2B0, hooked at its entry.
    //     The frame input update calls the keyboard poll (0x142728C40, a
    //     256-byte DirectInput key state) and the mouse poll (0x142728F20, a
    //     0x14-byte DIMOUSESTATE), then calls the fill with those buffers in
    //     r8 and rdx. The fill is their ONLY consumer: it resolves the
    //     player's in-game keymap against them and writes every
    //     keyStates_thisFrame entry, from which the bool→float mirrors
    //     (+0x274 LMB / +0x29C RMB / +0x2A8 Reload) and all edge/hold
    //     bookkeeping are derived. Clearing a byte there therefore removes the
    //     button from the whole frame in one place.
    //
    //   gamepad — the game's pad poll at 0x142728A1F, immediately after
    //     GetDeviceState has filled the 0x110-byte DirectInput state in rsi.
    //     This one function is the ONLY source of pad state on both of the
    //     game's pad paths (the device-9 keymap fill at 0x14273B390 writes
    //     InputContainerBig+0x864, the hard-coded per-device mapper at
    //     0x142739E40 reads a stack copy at rsp+0x20 — in both cases the very
    //     buffer rsi points at), so clearing a button here hides it from the
    //     keymap and the mapper alike: no action, no press edge, no hold.
    //
    // Previous sessions' approach — zeroing keyStates_thisFrame for selected
    // ActionKeyCode entries at three post-hoc sites (0x14273BC7B / 0x14273BEC5
    // / 0x14273A726) plus the pad snapshot at 0x14273BEB3 — is gone together
    // with the checkbox list that drove it. Those sites could only suppress an
    // *action* (they had to guess which pad button an action was bound to, and
    // the game resurrected the bytes from the pad stage), while blocking the
    // raw buffer makes the whole question moot: nothing downstream can write a
    // pressed state for a button the game never received.
    FreeJumpPlugin* g_ParkourModeInstance = nullptr;

    bool IsParkourBlockingActive()
    {
        const FreeJumpPlugin* owner = g_ParkourModeInstance;
        return owner && !owner->GetBlockedKeys().empty();
    }

    // The one mapping both blockers use: bindable code -> the byte(s) of the
    // game's own input buffers that must be cleared for it.
    BlockedInputSlot ComputeBlockedInputSlot(int bindableKeyCode)
    {
        switch (bindableKeyCode)
        {
        case VK_LBUTTON:
        case VK_RBUTTON:
        case VK_MBUTTON:
        case VK_XBUTTON1:
        case VK_XBUTTON2:
            return ComputeMouseButtonSlot(bindableKeyCode);
        default:
            break;
        }
        if (IsPadCode(bindableKeyCode))
            return ComputePadSnapshotSlot(bindableKeyCode);
        if (bindableKeyCode >= kKeyboardKeyMin && bindableKeyCode <= kKeyboardKeyMax)
            return ComputeKeyboardSlot(bindableKeyCode);
        return {};
    }

    // Keyboard/mouse blocker. Runs at the keymap fill's entry, with the
    // fill's own argument registers live: r8 = the keyboard state buffer the
    // keyboard poll just wrote (256 bytes, one byte per DirectInput scancode,
    // bit 7 = pressed) and rdx = the mouse state the mouse poll just wrote
    // (DIMOUSESTATE: lX/lY/lZ then rgbButtons at +0xC). Both buffers contain
    // only this frame's state, so clearing a byte here removes that key from
    // the entire frame — see the seam documentation above.
    void SanitizeKbmKeymapFillSource(AllRegisters* params)
    {
        if (!IsParkourBlockingActive())
            return;
        const FreeJumpPlugin* owner = g_ParkourModeInstance;
        auto* inputContainer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(params->rcx_));
        auto* keyboardState = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(params->r8_));
        auto* mouseState = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(params->rdx_));
        if (!inputContainer)
            return;
        // WHICH buffer r8 points at cannot be read off the statically
        // disassembled caller: the input update's bytes that set it sit at
        // 0x14273BC64, which another plugin's runtime trampoline has replaced
        // in the dump. So rather than guess, the write is verified against the
        // two buffers the frame's own code provably produces (the copies at
        // 0x14273BB4F/0x14273BB5E and the caller's `mov rdx,rsi` at
        // 0x14273BC6A). If a build ever hands the fill something else, the
        // callback declines to touch anything rather than corrupting an
        // unrelated buffer - Parkour Mode then stops blocking, it does not
        // crash. The offsets are the InputContainerBig fields the dump shows
        // the input update using; see the seam notes above.
        constexpr uintptr_t kMouseState_thisFrame = 0x22098;   // 0x14 bytes, DIMOUSESTATE
        constexpr uintptr_t kKbdState_thisFrame = 0x220C0;    // 0x100 bytes, scancodes
        constexpr uintptr_t kKbdState_prevFrame = 0x221C0;    // 0x100 bytes, scancodes
        const bool mouseStateIsFrameBuffer =
            mouseState == inputContainer + kMouseState_thisFrame;
        const bool keyboardStateIsFrameBuffer =
            keyboardState == inputContainer + kKbdState_thisFrame
            || keyboardState == inputContainer + kKbdState_prevFrame;
        if (!mouseStateIsFrameBuffer && !keyboardStateIsFrameBuffer)
            return;
        for (const int bindableKeyCode : owner->GetBlockedKeys())
        {
            const BlockedInputSlot slot = ComputeBlockedInputSlot(bindableKeyCode);
            switch (slot.kind)
            {
            case BlockedInputKind::KeyboardScancode:
                // Clear the whole byte, not just bit 7, so the key is not merely
                // unpressed but indistinguishable from "not touched this frame".
                if (keyboardStateIsFrameBuffer)
                    keyboardState[slot.slot] = 0;
                break;
            case BlockedInputKind::MouseButton:
                if (mouseStateIsFrameBuffer)
                    mouseState[0xC + slot.slot] = 0;   // DIMOUSESTATE rgbButtons
                break;
            default:
                break;
            }
        }
    }

    // ── Parkour Mode, gamepad: hide the button at ITS SOURCE ────────────────
    // 0x1427289A0 is the game's pad poll, and it is the ONE function both
    // gamepad paths go through:
    //   0x14273BEA1  call 0x1427289A0   device id 9  -> writes InputContainerBig+0x864
    //   0x142739EA6  call 0x1427289A0   every other id -> writes a stack copy at rsp+0x20
    // Both are a 0x110-byte DirectInput state whose digital buttons live at
    // +0x30+code (bit 7 = pressed) and whose hat dwords start at +0x20. Inside
    // the poll the destination buffer is rsi (set at 0x142728B8 from the
    // caller's rdx) and rbx is the InputContainerBig*.
    //
    // Hooking at 0x142728A1F - the instruction right after GetDeviceState
    // returned and filled the buffer - therefore lets one site clear the
    // buttons for BOTH paths, before the keymap fill (0x14273B390) or the
    // per-device mapper (0x142739E40) can turn them into actions. The game is
    // never handed the button at all: no action, no press edge, no hold.
    //
    // The rest of the poll function only reads the container's own analog
    // fields (+0x380) and the stack scratch area, never the buffer we clear,
    // so the force-feedback / vibration tail still works; and only the bytes
    // the player assigned are touched, so every other button and the sticks
    // stay fully alive.
    void SanitizePadPollSnapshot(AllRegisters* params)
    {
        if (!IsParkourBlockingActive())
            return;
        const FreeJumpPlugin* owner = g_ParkourModeInstance;
        auto* padState = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(params->rsi_));
        if (!padState)
            return;
        for (const int bindableKeyCode : owner->GetBlockedKeys())
        {
            const BlockedInputSlot slot = ComputeBlockedInputSlot(bindableKeyCode);
            switch (slot.kind)
            {
            case BlockedInputKind::PadSnapshotButton:
                // Clear the whole byte, not just bit 7, so the button is not
                // merely unpressed but indistinguishable from "not touched".
                padState[0x30 + slot.slot] = 0;
                break;
            case BlockedInputKind::PadSnapshotHat:
            {
                // DirectInput's "hat centered": every direction compare in the
                // mapper (0/9000/18000/27000) fails against it, which is what
                // frees all four d-pad directions in one write.
                *reinterpret_cast<uint32_t*>(padState + 0x20) = 0xFFFFFFFFu;
                break;
            }
            default:
                break;
            }
        }
    }

    // ── Parkour Mode, keyboard/mouse: clear the blocked key before the
    //    game's keymap fill can resolve it ─────────────────────────────
    // 0x14273B2B0 is that fill, entered once per frame from the input update
    // at 0x14273BC70 with:
    //   rcx = InputContainerBig*      (keyStates_thisFrame is rcx+0x30, 32 bytes)
    //   rdx = the mouse state buffer   (DIMOUSESTATE, buttons at +0xC)
    //   r8  = the keyboard state buffer (256 scancode bytes, bit 7 = pressed)
    //   r9  = the player's keymap      (32 entries, stride 0x214: type at +4,
    //                                   code at +8; type 1 = keyboard scancode,
    //                                   type 2 = mouse button index)
    // and it is the only reader of rdx/r8 this frame. Hooking its entry gives
    // the blocked key's byte to the callback in the fill's own registers, so
    // the fill then writes 0 into every keyStates_thisFrame entry that key
    // drives — no action, no analog mirror (+0x274 LMB / +0x29C RMB /
    // +0x2A8 Reload), no press edge, no held-button bookkeeping.
    struct ParkourModeKbmFillHook : AutoAssemblerCodeHolder_Base
    {
        ParkourModeKbmFillHook()
        {
            PresetScript_CCodeInTheMiddle(
                0x14273B2B0, 8,
                SanitizeKbmKeymapFillSource,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                true);
        }
    };

    AutoAssembleWrapper<ParkourModeKbmFillHook>& GetParkourModeKbmFillHook()
    {
        static AutoAssembleWrapper<ParkourModeKbmFillHook> s_hook;
        return s_hook;
    }

    // The single site that covers both gamepad paths: 0x142728A1F is
    // `movzx edi,dil; test eax,eax` (40 0F B6 FF | 85 C0) - 6 whole,
    // position-independent bytes immediately after `call [rax+0x48]`
    // (GetDeviceState, 0x110 bytes into rsi). rsi is this frame's pad
    // snapshot and rbx the InputContainerBig; both are callee-saved and used
    // again right after the site, so the interposed C call cannot have
    // clobbered them.
    struct ParkourModePadPollHook : AutoAssemblerCodeHolder_Base
    {
        ParkourModePadPollHook()
        {
            PresetScript_CCodeInTheMiddle(
                0x142728A1F, 6,
                SanitizePadPollSnapshot,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                true);
        }
    };

    AutoAssembleWrapper<ParkourModePadPollHook>& GetParkourModePadPollHook()
    {
        static AutoAssembleWrapper<ParkourModePadPollHook> s_hook;
        return s_hook;
    }

    // ── Parkour Mode: detach the game's pad device (fallback, off by default) ──
    // 0x1427289A0 is the frame poll of the game's DirectInput/HID pad state
    // (vtable slot 0x48 of the device at InputContainerBig+0x838, 0x110 bytes
    // into the caller's stack buffer). Both pad paths gate on its result:
    // returning 0 makes the device-9 keymap stage (0x14273BE6D) and the
    // hard-coded per-device-type mapper (0x142739E40, reached via jmp @
    // 0x14273BE68 for every device id != 9) bail out before they write a
    // single ActionKeyCode. Only the pad is affected: this function is the pad
    // stage's own device read (device object at InputContainerBig+0x838,
    // vtable slot 0x48, 0x110 bytes into the +0x864 buffer), while keyboard and
    // mouse come from the earlier KBM stage 0x14273B2B0, and the plugin's own
    // XInput polling (UpdatePadStates) is independent of the game's device, so
    // pad-bound override triggers keep working.
    //
    // Kept only as a fallback for builds whose pad input does not go through
    // the pad-poll site above (e.g. the UI reports that site as owned by
    // another plugin, and blocked pad buttons still reach the game). It
    // disables the ENTIRE controller, sticks included, so the character
    // cannot move while Parkour Mode is on — which is why it is off by
    // default instead of being the primary mechanism.
    // The whole function is replaced (no return path needed), and the original
    // prologue is restored on deactivate.
    struct ParkourModePadBlockHook : AutoAssemblerCodeHolder_Base
    {
        ParkourModePadBlockHook()
        {
            DEFINE_ADDR_NAMED(padPollAt, "ParkourModePadBlock", 0x1427289A0);
            ALLOC_NAMED(padBlockCave, "ParkourModePadBlockCave", 0x20, 0x1427289A0);
            padPollAt = {
                db(0xE9), RIP(padBlockCave),   // jmp cave
                nop(14 - 5),                   // overwrite the rest of the replaced prologue
            };
            padBlockCave = {
                "31 C0",                       // xor eax,eax  (looks like "no device")
                "C3",                          // ret
            };
        }
    };

    AutoAssembleWrapper<ParkourModePadBlockHook>& GetParkourModePadBlockHook()
    {
        static AutoAssembleWrapper<ParkourModePadBlockHook> s_hook;
        return s_hook;
    }

    // ── Wall-to-Hang (ported from ACUFixes-0.9.3 Parkour Helpers) ────────
    // While the dive hotkey is held, this makes the game create and select
    // wall-eject-to-hang moves. Seven hooks, all from the 0.9.3 source that is
    // validated against ACU 1.5.0:
    //   0x140135230 (12B) ClimbFacade scan also creates wallEjectToHang
    //   0x140137A84 (11B) BackEject scan also creates wallEjectToHang
    //   0x141A4D7CB (7B)  starting a walling cycle swaps the climbFacade
    //                     tester's fancy vtable to the baseclass that has
    //                     sidehop-to-swing (0x1439FA1A0)
    //   0x141A1BA0C (7B)  same for the failed-wallrun cycle
    //   0x140140E44 (10B) widen eject-to-hang ranges (returns to 0x140140EDB)
    //   0x140140DCA (10B) widen eject-to-hang ranges for ParkourUp
    //                     (returns to 0x140140FED)
    //   0x1401AD835 (7B)  flip the RTCP TargetAngle so backeject-to-hang picks
    //                     the backeject animation, not the sideeject one
    // The runtime dump shows all seven sites E9-patched when ACU_ParkourPatches
    // (which ships the same helpers) is installed, so installation refuses
    // cleanly per site and the UI says the feature is already provided.
    FreeJumpPlugin* g_WallToHangInstance = nullptr;
    class ParkourAction_Commonbase;

    DEFINE_GAME_FUNCTION(CreateParkourActionForParkourPointIfFits, 0x1401D1CF0, char, __fastcall,
        (EnumParkourAction a1, uint64 a2, __m128* a3, __m128* p_movementVecWorld_mb, float a5,
         int a6, char a7, uint64 a8, uint64 a9, uint64 p_currentLedge_mb,
         ParkourAction_Commonbase** p_newAction_out, float a12, float p_epsilon_mb));

    bool IsWallToHangSiteTaken(uintptr_t site)
    {
        // The only reliable signature we have for these sites: when another
        // plugin owns them, the first byte is the E9 of its jmp cave.
        return *reinterpret_cast<const uint8_t*>(site) == 0xE9;
    }

    void WhenScanClimbFacade_AlsoWallToHang(AllRegisters* params)
    {
        FreeJumpPlugin* owner = g_WallToHangInstance;
        if (!owner || !owner->ShouldForceWallToHang())
            return;
        bool isCreated = CreateParkourActionForParkourPointIfFits(
            EnumParkourAction::wallEjectToHang
            , params->rdx_
            , (__m128*)params->rdi_
            , (__m128*)params->r9_
            , params->XMM6.f0
            , (int&)params->r13_
            , *(char*)(params->GetRSP() + 0x100)
            , *(uint64*)(*(uint64*)params->r12_ + (int&)params->r15_)
            , *(uint64*)(params->rbp_ + 0x118)
            , *(uint64*)(params->rbp_ + 0x120)
            , (ParkourAction_Commonbase**)(params->GetRSP() + 0x108)
            , *(float*)(params->rbp_ + 0x104)
            , *(float*)(params->rbp_ + 0x108));
        if (isCreated)
        {
            ACU::Memory::SmallArrayAppend(*(SmallArray<ParkourAction_Commonbase*>*)params->rbx_,
                *(ParkourAction_Commonbase**)(params->GetRSP() + 0x108));
        }
    }

    void WhenScanBackEject_AlsoWallToHang(AllRegisters* params)
    {
        FreeJumpPlugin* owner = g_WallToHangInstance;
        if (!owner || !owner->ShouldForceWallToHang())
            return;
        bool isCreated = CreateParkourActionForParkourPointIfFits(
            EnumParkourAction::wallEjectToHang
            , *(uint64*)(params->GetRSP() + 0xE8)
            , (__m128*)params->rsi_
            , (__m128*)(params->rdi_ + 0x20)
            , params->XMM6.f0
            , (int&)params->r13_
            , *(char*)(params->GetRSP() + 0x110)
            , *(uint64*)(*(uint64*)params->r14_ + (int&)params->r12_)
            , *(uint64*)(params->rdi_ + 0x118)
            , *(uint64*)(params->rdi_ + 0x120)
            , (ParkourAction_Commonbase**)(params->GetRSP() + 0x70)
            , *(float*)(params->rdi_ + 0x104)
            , *(float*)(params->rdi_ + 0x108));
        if (isCreated)
        {
            ACU::Memory::SmallArrayAppend(*(SmallArray<ParkourAction_Commonbase*>*)params->rbx_,
                *(ParkourAction_Commonbase**)(params->GetRSP() + 0x70));
        }
    }

    // The two fancy vtables of the climbFacade tester, from the 0.9.3 source.
    constexpr uint64 kClimbFacadeFVTable_default = 0x1439E08D0;
    constexpr uint64 kClimbFacadeFVTable_sidehopToSwing = 0x1439FA1A0;

    void WhenStartingWallingCycle_SwapFVTable(AllRegisters* params)
    {
        FreeJumpPlugin* owner = g_WallToHangInstance;
        auto* hsWalling = (WhileOnWallSystem*)params->rbx_;
        if (!hsWalling || !hsWalling->climbFacade)
            return;
        uint64& climbFacadeFancyVTable = *(uint64*)((uint64)hsWalling->climbFacade + 8);
        climbFacadeFancyVTable = (owner && owner->ShouldForceWallToHang())
            ? kClimbFacadeFVTable_sidehopToSwing
            : kClimbFacadeFVTable_default;
    }

    void WhenStartingFailedWallrun_SwapFVTable(AllRegisters* params)
    {
        FreeJumpPlugin* owner = g_WallToHangInstance;
        uint64 hsFailedWallrun = params->rsi_;
        uint64 climbFacadeTester = *(uint64*)(hsFailedWallrun + 0x1D0);
        if (!climbFacadeTester)
            return;
        uint64& climbFacadeFancyVTable = *(uint64*)(climbFacadeTester + 8);
        climbFacadeFancyVTable = (owner && owner->ShouldForceWallToHang())
            ? kClimbFacadeFVTable_sidehopToSwing
            : kClimbFacadeFVTable_default;
    }

    void WidenEjectToHangRanges(AllRegisters* params)
    {
        FreeJumpPlugin* owner = g_WallToHangInstance;
        if (!owner || !owner->ShouldForceWallToHang())
            return;
        auto& action = *(AvailableParkourAction*)params->rdx_;
        action.heightDifferenceMin = -3;
        action.heightDifferenceMax = 1;
        action.expectedHeightDiff_mb = -2;
        action.expectedVerticalSpeed_mb = 9;
        action.expectedHorizontalSpeed = 4;
        action.expectedVerticalDefaultDisplace = -1;
        action.expectedDurationMaxExtension_mb = 1.1f;
        action.curveAllowedRangeMax = 2;
        action.curveAllowedRangeMin = -4;
        action.flt_228 = 1;
        action.expectedCurveRangeMin = -2;
        action.vec240.y = 1.7600001f;
        action.vec240.z = action.expectedVerticalSpeed_mb * 0.44000003f - 1.8972802f;
    }

    void WidenEjectToHangRanges_ParkourUp(AllRegisters* params)
    {
        FreeJumpPlugin* owner = g_WallToHangInstance;
        if (!owner || !owner->ShouldForceWallToHang())
            return;
        auto& action = *(AvailableParkourAction*)params->rdx_;
        action.heightDifferenceMin = -12;
        action.heightDifferenceMax = 1.5f;
        action.expectedHeightDiff_mb = 1.5f;
        action.expectedVerticalSpeed_mb = 11;
        action.expectedHorizontalSpeed = 4;
        action.expectedVerticalDefaultDisplace = -1;
        action.expectedDurationMin_mb = 0.7f;
        action.expectedDurationMaxExtension_mb = 1.8f;
        action.curveAllowedRangeMax = 2;
        action.curveAllowedRangeMin = -4;
        action.flt_228 = 1;
        action.expectedCurveRangeMin = -2;
        action.vec240.y = 2.8799999f;
        action.vec240.z = action.expectedVerticalSpeed_mb * 0.71999997f - 5.0803199f;
    }

    void FixRTCPTargetAngle_BackEjectToHang(AllRegisters* params)
    {
        FreeJumpPlugin* owner = g_WallToHangInstance;
        if (!owner || !owner->ShouldForceWallToHang())
            return;
        auto& action = *(AvailableParkourAction*)params->rbx_;
        if (action.GetEnumParkourAction() != EnumParkourAction::wallEjectToHang)
            return;
        float& calculatedRTCPTargetAngle = (float&)params->XMM0.f0;
        if (calculatedRTCPTargetAngle >= -180.0f && calculatedRTCPTargetAngle < -155.0f)
            calculatedRTCPTargetAngle = -calculatedRTCPTargetAngle;
    }

    struct WallToHangHooks : AutoAssemblerCodeHolder_Base
    {
        WallToHangHooks()
        {
            PresetScript_CCodeInTheMiddle(0x140135230, 12,
                WhenScanClimbFacade_AlsoWallToHang, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
            PresetScript_CCodeInTheMiddle(0x140137A84, 11,
                WhenScanBackEject_AlsoWallToHang, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
            PresetScript_CCodeInTheMiddle(0x141A4D7CB, 7,
                WhenStartingWallingCycle_SwapFVTable, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
            PresetScript_CCodeInTheMiddle(0x141A1BA0C, 7,
                WhenStartingFailedWallrun_SwapFVTable, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
            PresetScript_CCodeInTheMiddle(0x140140E44, 10,
                WidenEjectToHangRanges, 0x140140EDB, false);
            PresetScript_CCodeInTheMiddle(0x140140DCA, 10,
                WidenEjectToHangRanges_ParkourUp, 0x140140FED, false);
            PresetScript_CCodeInTheMiddle(0x1401AD835, 7,
                FixRTCPTargetAngle_BackEjectToHang, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
        }
    };

    AutoAssembleWrapper<WallToHangHooks>& GetWallToHangHooks()
    {
        static AutoAssembleWrapper<WallToHangHooks> s_hook;
        return s_hook;
    }

    constexpr uintptr_t kWallToHangSites[] = {
        0x140135230, 0x140137A84, 0x141A4D7CB, 0x141A1BA0C,
        0x140140E44, 0x140140DCA, 0x1401AD835,
    };

    // C-linkage entry so its address can be written into the fallback cave
    // without depending on how this TU mangles its local class members.
    extern "C" int ParkourOverrideFallbackDecide(AvailableParkourAction* action);

    // ── Override fallback when another plugin owns sort&select ───────────
    // ACUFixes' Parkour Helpers (dive, wall-to-hang) replace
    // SortAndSelectBestMatchingAction at 0x140133B00 with their own full
    // reimplementation, which only runs THEIR GenericHooksInParkourFiltering
    // instance's callbacks. Our ChooseBeforeFiltering then never executes and
    // every override stops working.
    //
    // The one call both reimplementations and the vanilla function share is the
    // per-candidate final filter: AvailableParkourAction__FinalFilter1
    // (0x1401D4DE0). It is only a 14-byte fancy-vtable dispatch thunk
    //   mov rax,[rcx+8] ; mov r10,[rax+0xED0] ; jmp r10
    // and the candidate order/fitness have already been decided by then, so a
    // forced "false" from here removes the candidate from consideration without
    // disturbing the caller. Since the thunk's last instruction is a tail jmp,
    // a cave that runs the stolen prologue needs no jmp back at all.
    //
    // Semantics mirror ChooseBeforeFiltering, applied per candidate:
    //   - Free Jump held  -> every candidate fails (equivalent to clearing the
    //     candidate list).
    //   - Snap Direction  -> candidates whose direction points away from the
    //     held input fail instead of having their fitness zeroed.
    //   - active override -> candidates whose type is not in the override's
    //     list fail when "Remove all other actions" is on; matching candidates
    //     get WidenAcceptanceRanges when "Expand acceptance range" is on. The
    //     fitness multiplier is intentionally NOT applied here: it is applied
    //     before the fitness sort in the normal path, and by this point the
    //     sort has already happened. With "Remove all other actions" (the
    //     default) selection is already deterministic, so only the
    //     remove-others-off case loses the multiplier, and the UI says so.
    struct ParkourOverrideFallbackHook : AutoAssemblerCodeHolder_Base
    {
        ParkourOverrideFallbackHook()
        {
            DEFINE_ADDR_NAMED(finalFilter1, "ParkourOverrideFallback", 0x1401D4DE0);
            ALLOC_NAMED(fallbackCave, "ParkourOverrideFallbackCave", 0x40, 0x1401D4DE0);
            // Direct rel32 call to the decision routine. DEFINE_ADDR takes a runtime
            // address, so this needs no intermediate pointer slot in the cave.
            DEFINE_ADDR_NAMED(decideFn, "ParkourOverrideFallbackDecideFn",
                reinterpret_cast<uintptr_t>(&ParkourOverrideFallbackDecide));
            LABEL(fallbackBlocked);
            finalFilter1 = {
                db(0xE9), RIP(fallbackCave),   // jmp cave
                nop(14 - 5),                   // the thunk is 14 bytes total
            };
            fallbackCave = {
                "48 83 EC 08",                     // sub rsp,8            (align for the call)
                db(0xE8), RIP(decideFn),            // call Decide          (rcx = the action)
                "48 83 C4 08",                     // add rsp,8
                "84 C0",                           // test al,al
                "0F 85", RIP(fallbackBlocked),     // jnz blocked
                "48 8B 41 08",                     // mov rax,[rcx+8]      (stolen)
                "4C 8B 90 D0 0E 00 00",            // mov r10,[rax+0xED0]  (stolen)
                "49 FF E2",                        // jmp r10              (stolen tail jmp)
                PutLabel(fallbackBlocked),
                "31 C0",                           // xor eax,eax
                "C3",                              // ret                  (AL = false)
            };
        }
    };

    AutoAssembleWrapper<ParkourOverrideFallbackHook>& GetParkourOverrideFallbackHook()
    {
        static AutoAssembleWrapper<ParkourOverrideFallbackHook> s_hook;
        return s_hook;
    }

    bool IsParkourOverrideFallbackSitePatched()
    {
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x1401D4DE0);
        return !(site[0] == 0x48 && site[1] == 0x8B && site[2] == 0x41 && site[3] == 0x08
            && site[4] == 0x4C && site[5] == 0x8B && site[6] == 0x90
            && site[7] == 0xD0 && site[8] == 0x0E && site[9] == 0x00 && site[10] == 0x00
            && site[11] == 0x49 && site[12] == 0xFF && site[13] == 0xE2);
    }

    // Each blocking site is asserted separately, so one already owned by
    // another plugin costs only its own device's blocking.
    bool IsKbmFillSitePatched()
    {
        // Vanilla prologue: lea r10,[rcx+0x30] ; add r9,8  (0x14273B2B0, 8 bytes).
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x14273B2B0);
        return !(site[0] == 0x4C && site[1] == 0x8D && site[2] == 0x51 && site[3] == 0x30
            && site[4] == 0x49 && site[5] == 0x83 && site[6] == 0xC1 && site[7] == 0x08);
    }

    bool IsPadPollSitePatched()
    {
        // Vanilla: movzx edi,dil ; test eax,eax  (0x142728A1F, 6 bytes).
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x142728A1F);
        return !(site[0] == 0x40 && site[1] == 0x0F && site[2] == 0xB6
            && site[3] == 0xFF && site[4] == 0x85 && site[5] == 0xC0);
    }

    bool IsParkourPadPollSitePatched()
    {
        // Pad poll header: push rbx; push rsi; push rdi; sub rsp,0x90.
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x1427289A0);
        return !(site[0] == 0x40 && site[1] == 0x53 && site[2] == 0x56 && site[3] == 0x57
            && site[4] == 0x48 && site[5] == 0x81 && site[6] == 0xEC && site[7] == 0x90);
    }
}

// ── Acceptance-range widening (ported from FailedWallRunPlugin) ──────
// Only-loosen semantics (never narrows vanilla) with the dump-proven
// permissive superset FailedWallRunPlugin uses. Widening runs in the GPH
// ChooseBeforeFiltering hook — after the game created the candidate
// actions and wrote their vanilla ranges, before the game's fitness and
// filtering passes consume them. Limitation (inherited from the
// FailedWallRun investigation): actions that vanilla's CREATION-time fit
// test already refused never appear in the candidate list, so they cannot
// be widened here — for type 52 specifically, FailedWallRunPlugin's own
// creation hooks remain the stronger tool.
constexpr float kRangeHeightDiffMin = -3.0f;
constexpr float kRangeHeightDiffMax = 12.0f;   // vanilla type-52 max is 5.0 — stay a superset
constexpr float kRangeHorizDiffMin = -3.0f;
constexpr float kRangeHorizDiffMax = 3.0f;
constexpr float kRangeDistMin = 0.0f;
constexpr float kRangeDistMax = 15.0f;         // vanilla type-52 max is 12.0 — stay a superset
constexpr float kRangeCurveMax = 2.0f;
constexpr float kRangeCurveMin = -4.0f;
constexpr float kRangeExpectedCurveMin = -2.0f;

void WidenMin(float& cur, float req) { if (req < cur) cur = req; }
void WidenMax(float& cur, float req) { if (req > cur) cur = req; }

void WidenAcceptanceRanges(AvailableParkourAction& action)
{
    WidenMin(action.heightDifferenceMin, kRangeHeightDiffMin);
    WidenMax(action.heightDifferenceMax, kRangeHeightDiffMax);
    WidenMin(action.horizontalDifferenceMin, kRangeHorizDiffMin);
    WidenMax(action.horizontalDifferenceMax, kRangeHorizDiffMax);
    WidenMin(action.distanceMin, kRangeDistMin);
    WidenMax(action.distanceMax, kRangeDistMax);
    WidenMax(action.curveAllowedRangeMax, kRangeCurveMax);
    WidenMin(action.curveAllowedRangeMin, kRangeCurveMin);
    WidenMin(action.expectedCurveRangeMin, kRangeExpectedCurveMin);
}

// ── INI persistence ───────────────────────────────────────────────────

static std::string GetIniPath()
{
    char documents[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, nullptr, 0, documents)))
    {
        std::string folder(documents);
        folder += "\\Assassin's Creed Unity";
        if (!CreateDirectoryA(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            // The stream operation below will report the actual write failure.
        }
        folder += "\\FreeJumpPlugin.ini";
        return folder;
    }
    return "FreeJumpPlugin.ini";
}

// ── Parkour Mode: what a blocked entry means in the game's own buffers ────
// Shown next to every entry of the blocked list, so the player can see that
// "Mouse Left" really is the game's button 0 and "Left Ctrl" really is
// scancode 0x1D rather than having to take the suppression on trust.
std::string FreeJumpPlugin::DescribeBlockedInputTarget(int bindableKeyCode)
{
    const BlockedInputSlot slot = ComputeBlockedInputSlot(bindableKeyCode);
    std::ostringstream where;
    switch (slot.kind)
    {
    case BlockedInputKind::KeyboardScancode:
        where << "keyboard scancode 0x" << std::uppercase << std::hex << slot.slot;
        break;
    case BlockedInputKind::MouseButton:
    {
        const char* buttonName = "extra button";
        switch (slot.slot)
        {
        case 0: buttonName = "left"; break;
        case 1: buttonName = "right"; break;
        case 2: buttonName = "middle"; break;
        case 3: buttonName = "X1"; break;
        case 4: buttonName = "X2"; break;
        default: break;
        }
        where << "mouse button " << slot.slot << " (" << buttonName << ")";
        break;
    }
    case BlockedInputKind::PadSnapshotButton:
        where << "controller button " << slot.slot
              << " (" << PadCodeName(bindableKeyCode) << ")";
        break;
    case BlockedInputKind::PadSnapshotHat:
        where << "controller d-pad (all four directions)";
        break;
    default:
        return "NOT blocked - the game reads this control as an analog axis, "
               "which this build does not map";
    }
    return where.str();
}

void FreeJumpPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;

    std::vector<ParkourActionOverride> loadedOverrides;
    std::vector<int> loadedBlockedKeys;
    bool hasBlockedKeys = false;   // distinguishes "list cleared" from "key absent"
    bool hasOverrideCount = false;
    bool hasOverrideData = false;
    std::string line;
    while (std::getline(file, line))
    {
        const std::string_view trimmedLine = Trim(line);
        std::string_view value;

        if (TryGetValue(trimmedLine, "FreeJumpKey=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidBindableKey(keyCode))
                m_ToggleKey = keyCode;
        }
        else if (TryGetValue(trimmedLine, "FreeJumpKeyPad=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidBindableKey(keyCode))
                m_ToggleKeyPad = keyCode;
        }
        else if (TryGetValue(trimmedLine, "ParkourModeKey=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidBindableKey(keyCode))
                m_ParkourModeKey = keyCode;
        }
        else if (TryGetValue(trimmedLine, "ParkourModeKeyPad=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidBindableKey(keyCode))
                m_ParkourModeKeyPad = keyCode;
        }
        else if (TryGetValue(trimmedLine, "UseControllerBindings=", value))
        {
            bool profile = false;
            if (TryParseBool(value, profile))
                m_UseControllerBindings = profile;
        }
        else if (TryGetValue(trimmedLine, "ParkourModeOn=", value))
        {
            bool enabled = false;
            if (TryParseBool(value, enabled))
                m_ParkourModeOn = enabled;
        }
        else if (TryGetValue(trimmedLine, "DetachPadFallback=", value))
        {
            bool enabled = false;
            if (TryParseBool(value, enabled))
                m_BlockPadWhileParkour = enabled;
        }
        // Legacy `AutoFallbackOnLeak=` is not read any more: it armed the pad-detach
        // fallback from the action-level leak detector, which no longer exists
        // (buttons are now blocked at their source). The manual "detach the
        // controller" fallback below is still available.
        // Legacy `BlockPadWhileParkour=` is deliberately NOT read: in 0.11 it
        // defaulted to 1 and was the primary (whole-pad) mechanism. Honouring a
        // stored 1 would silently re-enable the detach as the active fix and
        // freeze the sticks, hiding the per-site suppression. Unknown keys are
        // skipped, so old files load cleanly and the option is simply off.
        // Legacy `ParkourDisabledActions=` and `ParkourPadBindings=` are
        // deliberately NOT read: they described the old per-action checkbox
        // set, whose meaning depended on the in-game bindings of the moment it
        // was saved. Unknown keys are skipped, so an old file loads cleanly and
        // the fresh default set (LMB/RMB/Left Ctrl) is used instead.
        else if (TryGetValue(trimmedLine, "WallToHangEnabled=", value))
        {
            bool enabled = false;
            if (TryParseBool(value, enabled))
                m_WallToHangEnabled = enabled;
        }
        else if (TryGetValue(trimmedLine, "DiveHotkey=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidBindableKey(keyCode))
                m_DiveHotkey = keyCode;
        }
        else if (TryGetValue(trimmedLine, "DiveHotkeyPad=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidBindableKey(keyCode))
                m_DiveHotkeyPad = keyCode;
        }
        else if (TryGetValue(trimmedLine, "ParkourBlockedKeys=", value))
        {
            // Parkour Mode's blocked buttons: bindable codes (keyboard/mouse
            // Win32 virtual keys and controller codes), pipe-separated.
            // An EMPTY value is a meaningful state - the player deliberately
            // blocked nothing - so it must replace, not fall back to, the
            // built-in defaults.
            loadedBlockedKeys.clear();
            for (const std::string_view keyToken : Split(value, '|'))
            {
                int code = 0;
                if (TryParseInteger(keyToken, 10, code) && IsValidBindableKey(code))
                    loadedBlockedKeys.push_back(code);
            }
            hasBlockedKeys = true;
        }
        else if (TryGetValue(trimmedLine, "NumOverrides=", value))
        {
            int count = 0;
            if (TryParseInteger(value, 10, count) && count >= 0 && count <= kMaxOverrideCount)
            {
                if (!hasOverrideData)
                    loadedOverrides.assign(count, ParkourActionOverride{});
                else if (loadedOverrides.size() < static_cast<size_t>(count))
                    loadedOverrides.resize(count);
                hasOverrideCount = true;
                hasOverrideData = true;
            }
            else
            {
                // Preserve any valid Override records already read instead of
                // turning a malformed count into an empty configuration.
                hasOverrideCount = false;
            }
        }
        else if (TryGetValue(trimmedLine, "Override=", value))
        {
            int index = 0;
            ParkourActionOverride parsedOverride;
            if (!TryParseOverride(value, index, parsedOverride)
                || index < 0
                || index >= kMaxOverrideCount)
            {
                continue;
            }
            if (loadedOverrides.size() <= static_cast<size_t>(index))
                loadedOverrides.resize(static_cast<size_t>(index) + 1);
            loadedOverrides[static_cast<size_t>(index)] = std::move(parsedOverride);
            hasOverrideData = true;
        }
    }

    if (hasBlockedKeys)
        m_UserBlockedKeys = std::move(loadedBlockedKeys);
    if (hasOverrideCount || hasOverrideData)
        m_Overrides = std::move(loadedOverrides);
}

void FreeJumpPlugin::SaveSettings()
{
    const std::string path = GetIniPath();
    const std::string temporaryPath = path + ".tmp";

    std::ofstream file(temporaryPath, std::ios::out | std::ios::trunc);
    if (!file)
        return;

    file << "FreeJumpKey=" << std::hex << m_ToggleKey << std::dec << "\n";
    file << "FreeJumpKeyPad=" << std::hex << m_ToggleKeyPad << std::dec << "\n";
    file << "UseControllerBindings=" << (m_UseControllerBindings ? 1 : 0) << "\n";
    file << "ParkourModeKey=" << std::hex << m_ParkourModeKey << std::dec << "\n";
    file << "ParkourModeKeyPad=" << std::hex << m_ParkourModeKeyPad << std::dec << "\n";
    file << "ParkourModeOn=" << (m_ParkourModeOn ? 1 : 0) << "\n";
    file << "DetachPadFallback=" << (m_BlockPadWhileParkour ? 1 : 0) << "\n";
    // Parkour Mode's blocked buttons, as decimal bindable codes. Keyboard/mouse
    // keys and controller codes are both written, so a controller assignment
    // survives a restart exactly as it was assigned.
    // NB: the key name is new in 0.14. A pre-0.14 file (with the old
    // `UserBlockedKeys=` / `ParkourDisabledActions=` keys) has no
    // `ParkourBlockedKeys=` line at all, so it keeps the built-in defaults
    // instead of inheriting an obsolete or empty list.
    file << "ParkourBlockedKeys=";
    {
        bool firstKey = true;
        for (const int code : m_UserBlockedKeys)
        {
            if (!firstKey)
                file << "|";
            file << code;
            firstKey = false;
        }
    }
    file << "\n";
    file << "WallToHangEnabled=" << (m_WallToHangEnabled ? 1 : 0) << "\n";
    file << "DiveHotkey=" << std::hex << m_DiveHotkey << std::dec << "\n";
    file << "DiveHotkeyPad=" << std::hex << m_DiveHotkeyPad << std::dec << "\n";
    file << "NumOverrides=" << m_Overrides.size() << "\n";
    file << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (size_t i = 0; i < m_Overrides.size(); ++i)
    {
        const ParkourActionOverride& override = m_Overrides[i];
        file << "Override=" << i << "," << override.keyCode << ",";
        for (size_t actionIndex = 0; actionIndex < override.actionTypes.size(); ++actionIndex)
        {
            if (actionIndex > 0)
                file << "|";
            file << override.actionTypes[actionIndex];
        }
        file << "," << override.fitnessMultiplier
             << "," << (override.removeOtherActions ? 1 : 0)
             << "," << (override.enable ? 1 : 0)
             << "," << (override.toggleMode ? 1 : 0)
             << "," << (override.toggleState ? 1 : 0)
             << "," << override.padKeyCode
             << "," << (override.expandRanges ? 1 : 0) << ","
             << [&name = override.name]() {
                    std::string safe(name);
                    std::replace(safe.begin(), safe.end(), ',', ';');
                    return safe;
                }() << "\n";
    }

    file.flush();
    const bool flushSucceeded = file.good();
    file.close();
    if (!flushSucceeded || file.fail())
    {
        DeleteFileA(temporaryPath.c_str());
        return;
    }

    // Replace the old file only after the complete new file has been flushed.
    // This prevents a failed write from leaving a truncated configuration.
    if (!MoveFileExA(
            temporaryPath.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(temporaryPath.c_str());
    }
}

static Vector2f GetCameraFlatForward()
{
    auto* cam = ACUPlayerCameraComponent::GetSingleton();
    if (!cam) return { 0.0f, 1.0f };
    float x = *(float*)((uintptr_t)cam + 0x60);
    float y = *(float*)((uintptr_t)cam + 0x64);
    float z = *(float*)((uintptr_t)cam + 0x68);
    float w = *(float*)((uintptr_t)cam + 0x6C);
    float fx = 2.0f * (x*z + w*y);
    float fy = 2.0f * (y*z - w*x);
    return { fx, fy };
}

static void Rotate2D(float& x, float& y, float angle)
{
    float cosA = cosf(angle);
    float sinA = sinf(angle);
    float nx = x * cosA - y * sinA;
    float ny = x * sinA + y * cosA;
    x = nx; y = ny;
}

void FreeJumpPlugin::OnBeforeActivate()
{
    LoadSettings();

    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction*
    {
        return static_cast<FreeJumpPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.ChooseAfterSorting_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame) -> AvailableParkourAction*
    {
        return static_cast<FreeJumpPlugin*>(userData)->ChooseAfterSorting(actions, selectedByGame);
    };
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 5.0f;
    m_Callbacks.m_Name = "FreeJumpPlugin";

    // Parkour Mode installs only while it is on AND at least one button is
    // assigned; an empty list means nothing to block, so the hooks stay off.
    if (m_ParkourModeOn && !m_UserBlockedKeys.empty())
        ApplyParkourMode(true);
    if (m_WallToHangEnabled)
        ApplyWallToHang(true);
}

void FreeJumpPlugin::OnBeforeDeactivate()
{
    if (m_KbmFillSuppressActive || m_PadPollSanitizeActive || m_PadBlockHookActive)
        ApplyParkourMode(false);
    if (m_WallToHangHooksActive)
        ApplyWallToHang(false);
    if (m_OverrideFallbackActive)
    {
        GetParkourOverrideFallbackHook().Deactivate();
        ApplyOverrideFallback(false);
    }
    if (m_HookActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActive = false;
    }
    SaveSettings();
}

void FreeJumpPlugin::ApplyParkourMode(bool enable)
{
    if (enable)
    {
        // Every blocker reads this, so it has to be published before any hook
        // below can fire - including when one site is owned by someone else and
        // only the other family's site installs.
        g_ParkourModeInstance = this;

        // Two independent sites, one per device family. A site another plugin
        // already patched is skipped (never fought over) and costs only its own
        // family; the UI reports which are live, and the retry in OnUpdate
        // installs one that was refused earlier.
        bool anyRefused = false;
        if (!m_KbmFillSuppressActive)
        {
            if (IsKbmFillSitePatched())
                anyRefused = true;
            else
            {
                GetParkourModeKbmFillHook().Activate();
                m_KbmFillSuppressActive = true;
            }
        }
        // The pad path: one site at the poll covers both of the game's pad
        // stages, so the buttons themselves never reach the game.
        if (!m_PadPollSanitizeActive)
        {
            if (IsPadPollSitePatched())
                anyRefused = true;
            else
            {
                GetParkourModePadPollHook().Activate();
                m_PadPollSanitizeActive = true;
            }
        }
        m_ParkourHookRefused = anyRefused;

        // Optional fallback, off by default: detach the pad device. It hides
        // every pad button without a per-button mapping, but it also disables
        // the sticks, which is why it is not the primary mechanism.
        if (m_BlockPadWhileParkour && !m_PadBlockHookActive)
        {
            if (IsParkourPadPollSitePatched())
            {
                m_ParkourHookRefused = true;
            }
            else
            {
                GetParkourModePadBlockHook().Activate();
                m_PadBlockHookActive = true;
            }
        }
    }
    else
    {
        if (m_KbmFillSuppressActive)
        {
            GetParkourModeKbmFillHook().Deactivate();
            m_KbmFillSuppressActive = false;
        }
        if (m_PadPollSanitizeActive)
        {
            GetParkourModePadPollHook().Deactivate();
            m_PadPollSanitizeActive = false;
        }
        if (g_ParkourModeInstance == this)
            g_ParkourModeInstance = nullptr;
        if (m_PadBlockHookActive)
        {
            GetParkourModePadBlockHook().Deactivate();
            m_PadBlockHookActive = false;
        }
        m_ParkourHookRefused = false;
        ApplyOverrideFallback(false);
    }
}

void FreeJumpPlugin::ApplyWallToHang(bool enable)
{
    if (enable)
    {
        g_WallToHangInstance = this;
        // All-or-nothing: a partial install (e.g. creation hooks without the
        // vtable swaps) would create candidates the game then discards, which
        // looks like "the feature half-works". When ACU_ParkourPatches or
        // ACUFixes provides the same helpers, every site is E9-patched, we
        // install nothing, and the feature still works through that plugin.
        for (uintptr_t site : kWallToHangSites)
        {
            if (IsWallToHangSiteTaken(site))
            {
                m_WallToHangSiteTaken = true;
                m_WallToHangHooksActive = false;
                return;
            }
        }
        m_WallToHangSiteTaken = false;
        GetWallToHangHooks().Activate();
        m_WallToHangHooksActive = true;
    }
    else
    {
        if (m_WallToHangHooksActive)
        {
            GetWallToHangHooks().Deactivate();
            m_WallToHangHooksActive = false;
        }
        if (g_WallToHangInstance == this)
            g_WallToHangInstance = nullptr;
    }
}

bool FreeJumpPlugin::ShouldForceWallToHang() const
{
    if (!m_WallToHangEnabled)
        return false;
    const int effectiveDiveKey = EffectiveDiveHotkey();
    return IsValidBindableKey(effectiveDiveKey)
        && effectiveDiveKey != m_JustCapturedKey
        && !MouseBindWouldHitUi(effectiveDiveKey)
        && IsKeyHeld(effectiveDiveKey);
}

AvailableParkourAction* FreeJumpPlugin::ChooseAfterSorting(
    SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame)
{
    // Ported from ACUFixes' dive-move selection: while the dive hotkey is held,
    // the first available dive-family move wins over the game's own pick.
    // Return nullptr to leave the game's verdict untouched.
    if (!ShouldForceWallToHang())
        return nullptr;
    for (int i = 0; i < actions.size; ++i)
    {
        AvailableParkourAction* action = actions[i];
        if (!action)
            continue;
        const int actionType = GetTypeIntSafe(action);
        if (actionType == (int)EnumParkourAction::dive
            || actionType == (int)EnumParkourAction::swing_31_swingToSwing_mb
            || actionType == (int)EnumParkourAction::jumpOutOfWindowToHang
            || actionType == (int)EnumParkourAction::wallEjectToHang)
        {
            return action;
        }
    }
    return nullptr;
}

void FreeJumpPlugin::ApplyOverrideFallback(bool enable)
{
    if (enable != m_OverrideFallbackActive)
    {
        m_OverrideFallbackActive = enable;
        if (enable)
            g_OverrideFallbackInstance = this;
        else if (g_OverrideFallbackInstance == this)
            g_OverrideFallbackInstance = nullptr;
    }
}

extern "C" int ParkourOverrideFallbackDecide(AvailableParkourAction* action)
{
    return ParkourOverrideFallbackDecide_Impl(action);
}

bool FreeJumpPlugin::ShouldFallbackBlockCandidate(AvailableParkourAction* action)
{
    // Only meaningful when the normal callback path is unavailable; otherwise
    // the ChooseBeforeFiltering callback already did this work and would be
    // applied twice.
    if (m_HookActive)
        return false;

    // Free Jump: holding it clears the candidate list in the normal path.
    const int effectiveToggleKey = EffectiveToggleKey();
    if (!MouseBindWouldHitUi(effectiveToggleKey) && IsKeyHeld(effectiveToggleKey))
        return true;

    const int actionType = GetTypeIntSafe(action);

    // Active overrides.
    for (const ParkourActionOverride& ov : m_Overrides)
    {
        if (!ov.enable)
            continue;
        const int effectiveOverrideKey = EffectiveOverrideKey(ov);
        const bool active = ov.toggleMode ? ov.toggleState
            : (!MouseBindWouldHitUi(effectiveOverrideKey) && IsKeyHeld(effectiveOverrideKey));
        if (!active)
            continue;

        const bool typeMatches = actionType >= 0
            && std::find(ov.actionTypes.begin(), ov.actionTypes.end(), actionType) != ov.actionTypes.end();
        if (typeMatches && ov.expandRanges)
            WidenAcceptanceRanges(*action);
        if (ov.removeOtherActions && !typeMatches)
            return true;
    }
    return false;
}

void FreeJumpPlugin::UpdatePadStates()
{
    m_ActivePadSlot = -1;
    m_PadDown.fill(false);

    XInputGetState_t getState = GetXInputGetState();
    if (!getState)
    {
        m_PadDllLoaded = false;
        return;
    }
    m_PadDllLoaded = true;

    constexpr int kLeftThumbDeadzone = 7849;   // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
    constexpr int kTriggerThreshold = 30;      // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    constexpr int kPadSlotCount = 4;
    for (int slot = 0; slot < kPadSlotCount; ++slot)
    {
        XInputPadState state{};
        if (getState(slot, &state) != ERROR_SUCCESS)
            continue;

        auto markPadCode = [this](int padCode)
        {
            m_PadDown[padCode] = true;
        };

        if (state.buttons & 0x0001) markPadCode(PadCode_DPAD_UP);
        if (state.buttons & 0x0002) markPadCode(PadCode_DPAD_DOWN);
        if (state.buttons & 0x0004) markPadCode(PadCode_DPAD_LEFT);
        if (state.buttons & 0x0008) markPadCode(PadCode_DPAD_RIGHT);
        if (state.buttons & 0x0010) markPadCode(PadCode_START);
        if (state.buttons & 0x0020) markPadCode(PadCode_BACK);
        if (state.buttons & 0x0040) markPadCode(PadCode_L3);
        if (state.buttons & 0x0080) markPadCode(PadCode_R3);
        if (state.buttons & 0x0100) markPadCode(PadCode_LB);
        if (state.buttons & 0x0200) markPadCode(PadCode_RB);
        if (state.buttons & 0x1000) markPadCode(PadCode_A);
        if (state.buttons & 0x2000) markPadCode(PadCode_B);
        if (state.buttons & 0x4000) markPadCode(PadCode_X);
        if (state.buttons & 0x8000) markPadCode(PadCode_Y);
        if (state.leftTrigger > kTriggerThreshold) markPadCode(PadCode_LT);
        if (state.rightTrigger > kTriggerThreshold) markPadCode(PadCode_RT);
        if (state.thumbLY > kLeftThumbDeadzone)  markPadCode(PadCode_LSTICK_UP);
        if (state.thumbLY < -kLeftThumbDeadzone) markPadCode(PadCode_LSTICK_DOWN);
        if (state.thumbLX < -kLeftThumbDeadzone) markPadCode(PadCode_LSTICK_LEFT);
        if (state.thumbLX > kLeftThumbDeadzone)  markPadCode(PadCode_LSTICK_RIGHT);
        if (state.thumbRY > kLeftThumbDeadzone)  markPadCode(PadCode_RSTICK_UP);
        if (state.thumbRY < -kLeftThumbDeadzone) markPadCode(PadCode_RSTICK_DOWN);
        if (state.thumbRX < -kLeftThumbDeadzone) markPadCode(PadCode_RSTICK_LEFT);
        if (state.thumbRX > kLeftThumbDeadzone)  markPadCode(PadCode_RSTICK_RIGHT);

        if (state.buttons
            || state.leftTrigger > kTriggerThreshold
            || state.rightTrigger > kTriggerThreshold
            || state.thumbLX > kLeftThumbDeadzone || state.thumbLX < -kLeftThumbDeadzone
            || state.thumbLY > kLeftThumbDeadzone || state.thumbLY < -kLeftThumbDeadzone
            || state.thumbRX > kLeftThumbDeadzone || state.thumbRX < -kLeftThumbDeadzone
            || state.thumbRY > kLeftThumbDeadzone || state.thumbRY < -kLeftThumbDeadzone)
        {
            m_ActivePadSlot = slot;
        }
    }
}

void FreeJumpPlugin::UpdateKeyState(int bindableKeyCode)
{
    if (!IsValidBindableKey(bindableKeyCode) || m_KeyStateUpdatedThisFrame[bindableKeyCode])
        return;

    bool keyIsDown = false;
    if (bindableKeyCode <= kKeyboardKeyMax)
    {
        keyIsDown = (GetAsyncKeyState(bindableKeyCode) & 0x8000) != 0;
    }
    else
    {
        keyIsDown = m_PadDown[bindableKeyCode - kPadKeyBase];
    }
    m_KeyPressedThisFrame[bindableKeyCode] = keyIsDown && !m_PreviousKeyDown[bindableKeyCode];
    m_PreviousKeyDown[bindableKeyCode] = keyIsDown;
    m_KeyStateUpdatedThisFrame[bindableKeyCode] = true;
}

void FreeJumpPlugin::UpdateAllKeyStates()
{
    for (int vkCode = kKeyboardKeyMin; vkCode <= kKeyboardKeyMax; ++vkCode)
        UpdateKeyState(vkCode);
    for (int padCode = 0; padCode < PadCode_COUNT; ++padCode)
        UpdateKeyState(kPadKeyBase + padCode);
}

bool FreeJumpPlugin::WasKeyPressed(int bindableKeyCode) const
{
    return IsValidBindableKey(bindableKeyCode) && m_KeyPressedThisFrame[bindableKeyCode];
}

bool FreeJumpPlugin::IsKeyHeld(int bindableKeyCode) const
{
    if (IsPadCode(bindableKeyCode))
        return m_PadDown[bindableKeyCode - kPadKeyBase];
    if (bindableKeyCode >= kKeyboardKeyMin && bindableKeyCode <= kKeyboardKeyMax)
        return (GetAsyncKeyState(bindableKeyCode) & 0x8000) != 0;
    return false;
}

bool FreeJumpPlugin::IsSortAndSelectSiteAlreadyPatched() const
{
    // ACUFixes-0.9.3 and ACU_ParkourPatches replace the sort/select function
    // at this exact address. A second replacement would restore the other
    // plugin's bytes when either DLL unloads, leaving a dangling or vanilla
    // function under an active hook.
    constexpr uintptr_t kSortAndSelectSite = 0x140133B00;
    const uint16_t firstBytes = *reinterpret_cast<const uint16_t*>(kSortAndSelectSite);
    return (firstBytes & 0x00FF) == 0x00E9  // near JMP rel32
        || firstBytes == 0x25FF              // JMP qword ptr [RIP+disp32]
        || firstBytes == 0xB848;             // MOVABS RAX, imm64 patch prologue
}

void FreeJumpPlugin::OnUpdate()
{
    m_KeyStateUpdatedThisFrame.fill(false);
    m_JustCapturedKey = 0;
    UpdatePadStates();

    // Every binding exists in two profiles (keyboard/mouse + controller).
    // Both profiles' states are refreshed every frame so that switching
    // profiles mid-session can never leave a stale rising edge behind (the
    // stale-edge class of bug that once made Parkour Mode flip every frame).
    UpdateKeyState(m_ToggleKey);
    UpdateKeyState(m_ToggleKeyPad);
    // Without this refresh, m_KeyPressedThisFrame[m_ParkourModeKey] kept its
    // last value from whenever a rebind-wait last ran (it is only cleared by
    // UpdateKeyState). After the first rebind it stayed true forever, so
    // WasKeyPressed fired on every frame and Parkour Mode flipped ON/OFF each
    // frame — rewriting the INI and installing/removing the suppression hooks
    // at frame rate.
    UpdateKeyState(m_ParkourModeKey);
    UpdateKeyState(m_ParkourModeKeyPad);
    UpdateKeyState(m_DiveHotkey);
    UpdateKeyState(m_DiveHotkeyPad);
    for (const ParkourActionOverride& override : m_Overrides)
    {
        UpdateKeyState(override.keyCode);
        UpdateKeyState(override.padKeyCode);
    }

    const bool waitingForOverrideKey = std::any_of(
        m_Overrides.begin(),
        m_Overrides.end(),
        [](const ParkourActionOverride& override) { return override.waitingForKey; });
    const bool anyRebindWaiting = m_WaitingForKey || m_WaitingForDiveKey
        || m_WaitingForParkourKey || waitingForOverrideKey
        || m_WaitingForBlockedKey;
    if (anyRebindWaiting)
        UpdateAllKeyStates();

    // A rebind is always started by clicking a UI button, i.e. with a mouse
    // button. The starting click itself must not be captured as the new
    // binding: while a rebind is pending, LMB/RMB are only accepted after both
    // were seen released once since the rebind started.
    if (anyRebindWaiting && !m_AnyRebindWaitingLastFrame)
        m_MouseButtonsSettledSinceRebindStart = false;
    m_AnyRebindWaitingLastFrame = anyRebindWaiting;
    if (!m_MouseButtonsSettledSinceRebindStart
        && !IsKeyHeld(VK_LBUTTON)
        && !IsKeyHeld(VK_RBUTTON))
    {
        m_MouseButtonsSettledSinceRebindStart = true;
    }

    // Returns the code that was pressed. The caller decides which slot it
    // lands in, based on the device (below) — NOT on the active profile, which
    // previously made the profile switch behave inverted.
    const auto captureKey = [this](int& destinationKey) -> bool
    {
        for (int vkCode = kKeyboardKeyMin; vkCode <= kKeyboardKeyMax; ++vkCode)
        {
            // LMB/RMB are bindable like any other key. Skip them until the
            // mouse buttons settled (see above) so the click that opened the
            // rebind is never captured by accident. The middle and extra
            // mouse buttons, and all gamepad buttons, are always bindable.
            if ((vkCode == VK_LBUTTON || vkCode == VK_RBUTTON)
                && !m_MouseButtonsSettledSinceRebindStart)
            {
                continue;
            }
            if (WasKeyPressed(vkCode))
            {
                destinationKey = vkCode;
                return true;
            }
        }
        for (int padCode = 0; padCode < PadCode_COUNT; ++padCode)
        {
            if (WasKeyPressed(kPadKeyBase + padCode))
            {
                destinationKey = kPadKeyBase + padCode;
                return true;
            }
        }
        return false;
    };

    // Pad presses are stored in the controller slot and keyboard/mouse presses
    // in the keyboard/mouse slot, regardless of which profile is currently
    // active. That keeps both profiles populated while rebinding, which is what
    // makes the profile switch usable, and it is why the switch no longer
    // appears to behave inverted.
    const auto routeFeatureKey = [this, &captureKey](int& kbmSlot, int& padSlot) -> bool {
        int captured = 0;
        if (!captureKey(captured))
            return false;
        if (IsPadCode(captured))
            padSlot = captured;
        else
            kbmSlot = captured;
        return true;
    };

    if (m_WaitingForKey)
    {
        if (routeFeatureKey(m_ToggleKey, m_ToggleKeyPad))
        {
            m_JustCapturedKey = m_UseControllerBindings ? m_ToggleKeyPad : m_ToggleKey;
            m_WaitingForKey = false;
            SaveSettings();
        }
    }
    else if (m_WaitingForBlockedKey)
    {
        // Parkour Mode: the next button the player presses becomes blocked.
        // Keyboard keys, mouse buttons and pad buttons are all assignable; the
        // usual rebind guards (release latch for LMB/RMB) come from captureKey,
        // so the click that started this rebind cannot capture itself.
        int captured = 0;
        if (captureKey(captured))
        {
            if (std::find(m_UserBlockedKeys.begin(), m_UserBlockedKeys.end(), captured)
                == m_UserBlockedKeys.end())
            {
                m_UserBlockedKeys.push_back(captured);
            }
            m_JustCapturedKey = captured;
            m_WaitingForBlockedKey = false;
            SaveSettings();
        }
    }
    else if (m_WaitingForParkourKey)
    {
        if (routeFeatureKey(m_ParkourModeKey, m_ParkourModeKeyPad))
        {
            m_JustCapturedKey = m_UseControllerBindings ? m_ParkourModeKeyPad : m_ParkourModeKey;
            m_WaitingForParkourKey = false;
            SaveSettings();
        }
    }
    else if (m_WaitingForDiveKey)
    {
        if (routeFeatureKey(m_DiveHotkey, m_DiveHotkeyPad))
        {
            m_JustCapturedKey = m_UseControllerBindings ? m_DiveHotkeyPad : m_DiveHotkey;
            m_WaitingForDiveKey = false;
            SaveSettings();
        }
    }
    else if (waitingForOverrideKey)
    {
        for (ParkourActionOverride& override : m_Overrides)
        {
            if (!override.waitingForKey)
                continue;
            if (routeFeatureKey(override.keyCode, override.padKeyCode))
            {
                m_JustCapturedKey = EffectiveOverrideKey(override);
                override.waitingForKey = false;
                SaveSettings();
            }
            break;
        }
    }

    const int effectiveParkourKey = EffectiveParkourModeKey();
    if (WasKeyPressed(effectiveParkourKey) && effectiveParkourKey != m_JustCapturedKey
        && !MouseBindWouldHitUi(effectiveParkourKey))
    {
        m_ParkourModeOn = !m_ParkourModeOn;
        SaveSettings();
    }

    for (ParkourActionOverride& override : m_Overrides)
    {
        if (!override.enable || !override.toggleMode)
            continue;
        const int effectiveOverrideKey = EffectiveOverrideKey(override);
        if (WasKeyPressed(effectiveOverrideKey) && effectiveOverrideKey != m_JustCapturedKey
            && !MouseBindWouldHitUi(effectiveOverrideKey))
        {
            override.toggleState = !override.toggleState;
            SaveSettings();
        }
    }

    const int effectiveToggleKey = EffectiveToggleKey();
    const bool freeJumpHeld = effectiveToggleKey != m_JustCapturedKey
        && !MouseBindWouldHitUi(effectiveToggleKey)
        && IsKeyHeld(effectiveToggleKey);
    bool anyOverrideActive = false;
    for (const ParkourActionOverride& override : m_Overrides)
    {
        const int effectiveOverrideKey = EffectiveOverrideKey(override);
        const bool overrideKeyHeld = effectiveOverrideKey != m_JustCapturedKey
            && !MouseBindWouldHitUi(effectiveOverrideKey)
            && IsKeyHeld(effectiveOverrideKey);
        if (override.enable
            && (override.toggleMode
                ? override.toggleState
                : overrideKeyHeld))
        {
            anyOverrideActive = true;
        }
    }

    // Parkour Mode blocks buttons only while it is ON and the player has
    // assigned at least one; adding the first button (or toggling the mode on)
    // installs the sites, and the hooks come off the moment either stops being
    // true, so the game's input is untouched whenever the feature is unused.
    const bool wantParkourHook = m_ParkourModeOn && !m_UserBlockedKeys.empty();
    const bool anyParkourSuppressActive =
        m_KbmFillSuppressActive || m_PadPollSanitizeActive;
    // Re-run while anything is still missing, so a site that was refused once
    // (plugin loaded later, or a transient mismatch) is retried instead of
    // staying refused for the whole session.
    if (wantParkourHook
        && (!anyParkourSuppressActive
            || m_ParkourHookRefused
            || (m_BlockPadWhileParkour && !m_PadBlockHookActive)))
    {
        ApplyParkourMode(true);
    }
    else if (!wantParkourHook && (anyParkourSuppressActive || m_PadBlockHookActive))
    {
        ApplyParkourMode(false);
    }

    const bool needHook = freeJumpHeld || anyOverrideActive || (m_WallToHangEnabled && IsKeyHeld(EffectiveDiveHotkey()));

    // Normal path: our own sort&select replacement runs our ChooseBeforeFiltering
    // callback. When another plugin (ACUFixes' Parkour Helpers, e.g. dive and
    // wall-to-hang) already owns that function, we refuse it and fall back to
    // the FinalFilter1 cave below instead of silently doing nothing.
    if (needHook && !m_HookActive)
    {
        if (IsSortAndSelectSiteAlreadyPatched())
        {
            m_HookRefused = true;
        }
        else
        {
            m_HookRefused = false;
            auto gph = GenericHooksInParkourFiltering::GetSingleton();
            gph->Subscribe(m_Callbacks);
            m_HookActivator = gph->RequestGPHSortAndSelect();
            m_HookActive = true;
        }
    }
    else if (!needHook && m_HookActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActive = false;
    }

    // Fallback path: same trigger condition as the hook above, but active only
    // while the sort&select site is owned by someone else. Installing it at any
    // other time would double-apply our own effects.
    const bool wantFallback = needHook && m_HookRefused;
    if (wantFallback && !m_OverrideFallbackActive)
    {
        if (IsParkourOverrideFallbackSitePatched())
        {
            // Even the fallback site is taken; nothing more we can do safely.
            m_OverrideFallbackRefused = true;
        }
        else
        {
            m_OverrideFallbackRefused = false;
            GetParkourOverrideFallbackHook().Activate();
            ApplyOverrideFallback(true);
        }
    }
    else if (!wantFallback && m_OverrideFallbackActive)
    {
        GetParkourOverrideFallbackHook().Deactivate();
        ApplyOverrideFallback(false);
    }
}

void FreeJumpPlugin::OnImGuiRender()
{
    // The indicator must use the same activation semantics as OnUpdate: the
    // effective key of the ACTIVE profile, ignoring a key a rebind just
    // captured, and never reporting a mouse binding as held while ImGui owns
    // the cursor. The old IsKeyHeld(m_ToggleKey) always read the keyboard
    // slot, so with controller bindings selected the ON/OFF tag froze.
    const int freeJumpKey = EffectiveToggleKey();
    const bool freeMode = freeJumpKey != m_JustCapturedKey
        && !MouseBindWouldHitUi(freeJumpKey)
        && IsKeyHeld(freeJumpKey);
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
    const ImVec4 holdColor(0.20f, 0.70f, 1.00f, 1.00f);
    const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);

    ImGui::Text("FreeJumpPlugin");
    if (m_HookRefused && m_OverrideFallbackRefused)
        ImGui::TextColored(inactiveColor, "Overrides OFF: another plugin owns both the sort site and the compatibility hook.");
    else if (m_HookRefused)
        ImGui::TextDisabled("Compatibility mode: overrides active; per-override fitness multiplier not applied.");
    if (m_ParkourHookRefused)
        ImGui::TextColored(inactiveColor, "Parkour Mode: one input site is owned by another plugin; that device's suppression is missing.");
    if (m_WallToHangSiteTaken)
        ImGui::TextDisabled("Wall-to-Hang: already provided by another plugin.");
    if (m_PadDllLoaded && m_ActivePadSlot >= 0)
        ImGui::Text("Gamepad: connected (slot %d)", m_ActivePadSlot);
    if (ImGui::CollapsingHeader("Basic Features", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // ── Binding profile ─────────────────────────────────────────────
        // One switch swaps every feature and override between the pair of
        // bindings the player set once for each device, so switching from
        // keyboard/mouse to a controller never requires rebinding a single
        // action. Both profiles are polled every frame regardless of which is
        // active, so no stale key state can survive the switch.
        bool controllerBindings = m_UseControllerBindings;
        if (ImGui::Checkbox("Use controller bindings", &controllerBindings))
        {
            m_UseControllerBindings = controllerBindings;
            m_JustCapturedKey = 0;
            SaveSettings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(m_UseControllerBindings ? "(profile: CONTROLLER)" : "(profile: KEYBOARD/MOUSE)");

        ImGui::Separator();
        ImGui::Text("Free Jump:");
        ImGui::SameLine();
        ImGui::TextColored(freeMode ? activeColor : inactiveColor, freeMode ? "ACTIVE (HOLD)" : "OFF");
        ImGui::Text("  Bound key: %s", KeyCodeLabel(EffectiveToggleKey()).c_str());
        if (m_WaitingForKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Input key to bind..."); }
        if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind##FreeJump"))
        {
            m_WaitingForKey = !m_WaitingForKey;
            if (m_WaitingForKey)
            {
                m_WaitingForParkourKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
            }
        }

        // Wall-to-Hang: dive key (keyboard/mouse profile + controller profile).
        // The user binds the physical controller button, not the in-game action.
        ImGui::SameLine();
        ImGui::TextColored(m_WallToHangEnabled ? activeColor : inactiveColor,
                           m_WallToHangEnabled ? "ON" : "OFF");
        ImGui::Text("  Hotkey: %s", KeyCodeLabel(EffectiveDiveHotkey()).c_str());
        if (ImGui::Button(m_WaitingForDiveKey ? "Cancel" : "Rebind Dive"))
        {
            m_WaitingForDiveKey = !m_WaitingForDiveKey;
            if (m_WaitingForDiveKey)
            {
                m_WaitingForKey = false;
                m_WaitingForParkourKey = false;
                m_WaitingForBlockedKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
            }
            else
                SaveSettings();
        }
        if (m_WaitingForDiveKey)
            ImGui::TextColored(rebindColor, "Press any controller button or keyboard key to bind...");

        ImGui::Separator();
        ImGui::Text("Parkour Mode:");
        ImGui::SameLine();
        ImGui::TextColored(m_ParkourModeOn ? activeColor : inactiveColor,
                           m_ParkourModeOn ? "ON" : "OFF");
        if (m_ParkourModeOn)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(hides the blocked buttons below from the game)");
        }
        if (m_ParkourModeOn && m_UserBlockedKeys.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(inactiveColor, "- nothing assigned yet");
        }
        ImGui::Text("  Toggle key: %s", KeyCodeLabel(EffectiveParkourModeKey()).c_str());
        if (m_WaitingForParkourKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Input key or pad button to bind..."); }
        if (ImGui::Button(m_WaitingForParkourKey ? "Cancel Rebind##ParkourMode" : "Rebind##ParkourMode"))
        {
            m_WaitingForParkourKey = !m_WaitingForParkourKey;
            if (m_WaitingForParkourKey)
            {
                m_WaitingForKey = false;
                m_WaitingForBlockedKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
            }
        }

        // ── Blocked buttons ──────────────────────────────────────────────
        // The list is the whole feature: every button added here is erased
        // from the game's own copy of this frame's input before the game reads
        // it, so the game behaves as if that key/button simply does not exist
        // while Parkour Mode is ON - while the plugin keeps reading it, so the
        // very same button can drive a parkour override at that moment.
        ImGui::TextWrapped(
            "While Parkour Mode is ON, the game cannot see any button listed here. "
            "Assign the buttons whose in-game actions you want freed; the button "
            "itself keeps working for this plugin's overrides.");
        if (ImGui::TreeNode("Buttons blocked while Parkour Mode is on"))
        {
            bool listChanged = false;
            for (int i = 0; i < (int)m_UserBlockedKeys.size(); ++i)
            {
                const int code = m_UserBlockedKeys[i];
                ImGui::PushID(i);
                ImGui::TextUnformatted(KeyCodeLabel(code).c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("-> %s", DescribeBlockedInputTarget(code).c_str());
                ImGui::SameLine();
                if (ImGui::Button("Remove"))
                {
                    m_UserBlockedKeys.erase(m_UserBlockedKeys.begin() + i);
                    listChanged = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (m_UserBlockedKeys.empty())
                ImGui::TextColored(inactiveColor, "Nothing is blocked yet.");
            if (ImGui::Button(m_WaitingForBlockedKey ? "Cancel Add" : "Add button to block..."))
            {
                // Cancel every other rebind first: a button that is pressed
                // while another rebind is waiting must not be captured twice.
                m_WaitingForKey = false;
                m_WaitingForParkourKey = false;
                m_WaitingForDiveKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
                m_WaitingForBlockedKey = !m_WaitingForBlockedKey;
            }
            if (m_WaitingForBlockedKey)
            {
                ImGui::SameLine();
                ImGui::TextColored(rebindColor, "Press any key, mouse button or controller button...");
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear all") && !m_UserBlockedKeys.empty())
            {
                m_UserBlockedKeys.clear();
                listChanged = true;
            }
            if (listChanged)
                SaveSettings();
            // Transparency about what cannot be blocked: stick directions and
            // triggers arrive as analog axes, and this build has not mapped
            // them, so the UI says so instead of silently ignoring them.
            ImGui::TextDisabled(
                "Stick directions and the triggers cannot be blocked (the game reads them as "
                "analog axes); every other keyboard key, mouse button, controller button and "
                "the d-pad can.");
            ImGui::TreePop();
        }
        bool blockPad = m_BlockPadWhileParkour;
        if (ImGui::Checkbox("Fallback: detach the controller (also stops stick movement)", &blockPad))
        {
            m_BlockPadWhileParkour = blockPad;
            SaveSettings();
            if (m_ParkourModeOn)
                ApplyParkourMode(false), ApplyParkourMode(true);
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Parkour Action Overrides", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("+ Add Custom Override")
            && m_Overrides.size() < static_cast<size_t>(kMaxOverrideCount))
        {
            ParkourActionOverride newOverride;
            newOverride.keyCode = VK_F9; // sensible default, user rebinds after
            newOverride.waitingForKey = true; // start in bind mode
            m_WaitingForKey = false;
            m_WaitingForParkourKey = false;
            m_WaitingForBlockedKey = false;
            for (ParkourActionOverride& override : m_Overrides)
                override.waitingForKey = false;
            m_Overrides.push_back(newOverride);
            SaveSettings();
        }
        if (m_Overrides.empty())
            ImGui::TextColored(inactiveColor, "No custom overrides defined.");
        else
        {
            for (int idx = 0; idx < (int)m_Overrides.size(); ++idx)
            {
                auto& ov = m_Overrides[idx];
                ImGui::PushID(idx);
                const int effectiveKey = EffectiveOverrideKey(ov);
                bool active = ov.enable && (ov.toggleMode ? ov.toggleState
                    : (IsValidBindableKey(effectiveKey) && IsKeyHeld(effectiveKey)
                        && !MouseBindWouldHitUi(effectiveKey)));
                ImGui::TextColored(active ? holdColor : inactiveColor, "[%s]",
                                   active ? "ACTIVE" : (ov.enable ? "ENABLED" : "DISABLED"));
                ImGui::SameLine();
                // Displayed label: the user-editable name, or the generated
                // fallback so unnamed legacy overrides stay identifiable.
                const std::string displayName = ov.name.empty()
                    ? ("Custom " + std::to_string(idx))
                    : ov.name;
                if (ImGui::CollapsingHeader(displayName.c_str()))
                {
                    if (ImGui::Checkbox("Enable", &ov.enable))
                        SaveSettings();
                    ImGui::SameLine();
                    const int effectiveKey = EffectiveOverrideKey(ov);
                    ImGui::Text("Key: %s%s", KeyCodeLabel(effectiveKey).c_str(),
                                 IsValidBindableKey(effectiveKey) ? "" : " (unbound!)");
                    ImGui::SameLine();
                    if (ImGui::Button(ov.waitingForKey ? "Cancel" : "Rebind"))
                    {
                        const bool startWaiting = !ov.waitingForKey;
                        for (ParkourActionOverride& otherOverride : m_Overrides)
                            otherOverride.waitingForKey = false;
                        ov.waitingForKey = startWaiting;
                        if (startWaiting)
                        {
                            m_WaitingForKey = false;
                            m_WaitingForParkourKey = false;
                            m_WaitingForBlockedKey = false;
                        }
                    }
                    if (ov.waitingForKey) ImGui::TextColored(rebindColor, "Input key or pad button to bind...");

                    bool holdMode = !ov.toggleMode;
                    if (ImGui::RadioButton("Hold (press and hold)", holdMode))
                    {
                        ov.toggleMode = false;
                        SaveSettings();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Toggle (press once to lock)", ov.toggleMode))
                    {
                        ov.toggleMode = true;
                        SaveSettings();
                    }

                    {
        const char* kActionNames[] = {
            "stumbleAtEdge_mb (1)", "slideUnder2 (2)", "stepDownSmallHeight_mb (3)", "leapBeamToVShape (4)",
            "fromVshapeSmallStepDown (5)", "fromSlopeRiseToGround (6)", "stepDownWaistHeight_mb (8)",
            "riseFromGroundToGround_smallDiff_alsoVault9 (9)", "passementFromBeam_A (10)",
            "fromGroundVaultThroughVShape (11)", "passementFromBeam_C (12)", "passementD (13)",
            "riseFromGroundToGround_smallDiff_alsoVault14 (14)", "jumpDownToGround_airtimeButSafe (15)",
            "wallEjectToHang (17)", "breakfall (18)", "freefallLedgeLandOn (20)", "breakfall21 (21)",
            "fromHangToWallUpSameDepth_mb (23)", "fromHangToWall_side (26)",
            "fromHangToWallingUpAndTurnCornerOutside (27)", "hangTurnCornerInside (28)",
            "hangTurnCornerOutside (29)", "hangToDropDown_safe (31)", "riseFromHang (32)",
            "moveToSideWhileStillHanging (33)", "hangTurn_22 (34)", "jumpAndGrabFrontWall (35)",
            "offTheWall_fromWallToGroundReleaseOrEject (36)", "fromGroundToLadder_up (37)",
            "runOffGroundEdgeAndGrabSideWall (38)",
            "climbFacade_fromWallDescentSidewaysToGround_alsoSidehop (39)", "dive (40)",
            "leapOffBuildingIntoUncontrolledFall_mb (41)", "leapOfFaithIntoWaterViaParkourUp (42)",
            "swing_2B (43)", "riseFromHangHP_mb (44)", "swingTurn_2D (45)", "swingToWalling_front (46)",
            "swing_2F_SwingToSidewall_includingSpindescent (47)", "swing_30_fromSwingToFeet (48)",
            "swing_31_swingToSwing_mb (49)", "offTheWall_fromWallToWallEjectAndCatch (50)",
            "wallrunUpFromGroundFailed_mb (52)", "wallrunUpFromGround_mb (53)", "wallrunUpToHang (54)",
            "fromSlopeRiseToWall (55)", "slideUnder56 (56)", "plainStepDownToHang (57)",
            "fromSlopeSlideToHang (58)", "outOfWindowToHang_almostFellOutOfWindow (59)",
            "stepDownFromBeamToWall_mb (60)", "fromGroundToLadder_down (61)",
            "fromSlopeToWall_side_mb___UNABLE_TO_TRIGGER (62)",
            "edgeDropBeforeFreeeFall_3F___UNABLE_TO_TRIGGER (63)",
            "fromGroundTurnAroundAndGrabWallParkourDown (64)", "onWallNormalClimb (65)",
            "climbCornerInside_onWallTurnCornerInner (67)", "onWallTurnCornerOuter (68)",
            "plainDismountFromWall_mb (69)", "fromWallToHang_down (70)", "fromWallToHang_side (71)",
            "fromWallingToHangDownAndTurnCornerOutside (72)", "climb_fromWallRiseToFeetOnLedge_49 (73)",
            "riseToVShape (75)", "fromLadderToGround_up (76)", "fromWallToHang_up (77)",
            "fromHangToWall_down (78)", "backEject_4F (79)", "onLadderUpDown (80)", "choke_mb (86)",
            "fromWaterToWall (97)", "walkingOnBeamPassColumnAndContinue_R (100)",
            "walkingOnBeamPassColumnAndContinue_L (101)", "walkThroughWindow_mb_106 (106)",
            "flipOverWindowLedgeToWalling (107)", "enterWindow (108)", "leapOfFaith (112)",
            "passementFromHayStack (114)", "startJumpOutOfWindow (115)",
            "jumpOutOfWindowDescendToLedge_mb (116)", "jumpOutOfWindowToFeet (119)",
            "jumpOutOfWindowToWalling (120)", "jumpOutOfWindowToHang (121)",
            "defenestrateSprintOutOfWindow_122 (122)", "jumpIntoWindow (123)",
        };
        int kActionValues[] = {
            1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 20, 21, 23, 26, 27, 28, 29, 31, 32, 33,
            34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 52, 53, 54, 55, 56, 57, 58,
            59, 60, 61, 62, 63, 64, 65, 67, 68, 69, 70, 71, 72, 73, 75, 76, 77, 78, 79, 80, 86, 97, 100, 101,
            106, 107, 108, 112, 114, 115, 116, 119, 120, 121, 122, 123,
        };
                        ImGui::Text("Action Types:");
                        for (int t = 0; t < (int)ov.actionTypes.size(); ++t)
                        {
                            ImGui::PushID(t);
                            if (ImGui::InputInt("##Type", &ov.actionTypes[t]))
                                SaveSettings();
                            ImGui::SameLine();
                            int presetIdx = -1;
                            for (int pi = 0; pi < IM_ARRAYSIZE(kActionValues); ++pi)
                                if (kActionValues[pi] == ov.actionTypes[t]) { presetIdx = pi; break; }
                            if (ImGui::Combo("##Preset", &presetIdx, kActionNames, IM_ARRAYSIZE(kActionNames)))
                            {
                                if (presetIdx >= 0 && presetIdx < IM_ARRAYSIZE(kActionValues))
                                    ov.actionTypes[t] = kActionValues[presetIdx];
                                SaveSettings();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("X"))
                            {
                                ov.actionTypes.erase(ov.actionTypes.begin() + t);
                                SaveSettings();
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::Button("+ Add Action Type"))
                        {
                            ov.actionTypes.push_back(50);
                            SaveSettings();
                        }
                    }
                    if (ImGui::DragFloat("Fitness Multiplier", &ov.fitnessMultiplier, 0.5f, 0.0f, 100000.0f))
                        SaveSettings();
                    if (ImGui::Checkbox("Remove all other actions", &ov.removeOtherActions))
                        SaveSettings();
                    if (ImGui::Checkbox("Expand acceptance range", &ov.expandRanges))
                        SaveSettings();
                    if (ImGui::Button("Delete Override"))
                    {
                        m_Overrides.erase(m_Overrides.begin() + idx);
                        SaveSettings();
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }
        }
    }
}

AvailableParkourAction* FreeJumpPlugin::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    // Free Jump
    const int effectiveToggleKey = EffectiveToggleKey();
    if (!MouseBindWouldHitUi(effectiveToggleKey) && IsKeyHeld(effectiveToggleKey))
    {
        actions.size = 0;
        return nullptr;
    }

    // Custom overrides — collect all active override entries
    struct ActiveOverride { const ParkourActionOverride* ptr; };
    std::vector<ActiveOverride> actives;
    for (auto& ov : m_Overrides)
    {
        if (!ov.enable) continue;
        const int effectiveOverrideKey = EffectiveOverrideKey(ov);
        bool active = ov.toggleMode ? ov.toggleState
            : (!MouseBindWouldHitUi(effectiveOverrideKey) && IsKeyHeld(effectiveOverrideKey));
        if (!active) continue;
        actives.push_back({&ov});
    }

    if (!actives.empty())
    {
        // Build union of allowed action types across all active overrides
        std::vector<int> allowedTypes;
        bool shouldRemoveOthers = false;
        for (auto& ao : actives)
        {
            for (int type : ao.ptr->actionTypes)
                if (std::find(allowedTypes.begin(), allowedTypes.end(), type) == allowedTypes.end())
                    allowedTypes.push_back(type);
            if (ao.ptr->removeOtherActions)
                shouldRemoveOthers = true;
        }

        // Remove actions whose type is not in the allowed set
        if (shouldRemoveOthers)
        {
            for (int i = actions.size - 1; i >= 0; --i)
            {
                AvailableParkourAction* action = actions[i];
                if (!action) continue;
                if (std::find(allowedTypes.begin(), allowedTypes.end(),
                    GetTypeIntSafe(action)) == allowedTypes.end())
                    SmallArray_POD__RemoveGeneric(&actions, i, sizeof(AvailableParkourAction*));
            }
        }

        // Per-override acceptance-range widening (only-loosen, FailedWallRun
        // constants — see WidenAcceptanceRanges). Runs before the game's
        // fitness/filtering passes consume the ranges.
        for (const ActiveOverride& ao : actives)
        {
            if (!ao.ptr->expandRanges)
                continue;
            for (int i = 0; i < actions.size; ++i)
            {
                AvailableParkourAction* action = actions[i];
                if (!action) continue;
                const int actionType = GetTypeIntSafe(action);
                if (actionType < 0
                    || std::find(ao.ptr->actionTypes.begin(), ao.ptr->actionTypes.end(), actionType)
                        == ao.ptr->actionTypes.end())
                {
                    continue;
                }
                WidenAcceptanceRanges(*action);
            }
        }

        // Apply fitness multipliers for matching actions
        for (auto& ao : actives)
        {
            for (int i = 0; i < actions.size; ++i)
            {
                AvailableParkourAction* action = actions[i];
                if (!action) continue;
                if (std::find(ao.ptr->actionTypes.begin(), ao.ptr->actionTypes.end(),
                    GetTypeIntSafe(action)) != ao.ptr->actionTypes.end())
                {
                    float* fitness = (float*)((uintptr_t)action + 0x204);
                    if (ao.ptr->fitnessMultiplier != 1.0f)
                        *fitness *= ao.ptr->fitnessMultiplier;
                }
            }
        }
    }
    return nullptr;
}
