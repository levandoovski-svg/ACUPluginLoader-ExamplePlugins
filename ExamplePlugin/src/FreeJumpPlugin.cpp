#include "pch.h"
#include "FreeJumpPlugin.h"
#include "ACU/ACUPlayerCameraComponent.h"
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

// NOTE: AvailableParkourAction::GetEnumParkourAction() is defined exactly once,
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

    const char* ActionKeyName(int actionIdx)
    {
        static const char* names[kActionKeyCount] = {
            "ParkourUp (Space)",
            "LMB (Attack)",
            "EagleVision",
            "ParkourDownInteract",
            "Disguise",
            "Numpad4 CycleLeftHand",
            "Medicine",
            "Numpad6 CycleBombs",
            "Map",
            "Escape",
            "Crouch",
            "RMB (Aim)",
            "Sprint",
            "BombDrop",
            "Reload",
            "CenterCamera",
            "SlotPhantom",
            "SlotBerserk",
            "SlotGun",
            "SlotSmokeBomb",
            "SlotStunBomb",
            "SlotCherryBomb",
            "SlotPoisonBomb",
            "SlotMoneyPouch",
            "Slot9_booster",
        };
        if (actionIdx < 0 || actionIdx >= kActionKeyCount)
            return "";
        // Slots beyond the initializer list are nullptr; dereferencing them
        // crashed the disabled-actions UI the moment its tree was opened.
        const char* name = names[actionIdx];
        return name ? name : "";
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

    // ── Parkour mode: action-key suppression ──────────────────────
    // Parkour Mode frees buttons by forcing the selected ActionKeyCode entries
    // of InputContainerBig unpressed (keyStates_thisFrame, +0x30, 32 bytes).
    // The game writes those same 32 bytes THREE times per frame, and a single
    // hook after the first write can only ever cover the first one:
    //
    //   0x14273B2B0  keyboard/mouse keymap fill      (called at 0x14273BC70)
    //   → site K, 0x14273BC7B                        (zero here: keyboard/mouse)
    //   0x14273C190  mouse vectors + bool→float conversion
    //   …
    //   0x14273BE10  gamepad stage — a separate function, so it always runs
    //                after everything above and rewrites keyStates again:
    //                  device id 9 → keymap fill 0x14273B390, then
    //                                → site P, 0x14273BEC5
    //                                → 0x14273C4C0 and the bool→float conversion
    //                  any other id → jmp 0x142739E40 (hard-coded per-device
    //                                mapper; its case bodies write
    //                                keyStates[0..15] directly), cases converge
    //                                at 0x14273A71E, then
    //                                → site M, 0x14273A726
    //                                → bool→float conversion at 0x14273A736
    //
    // That ordering is why controller buttons stayed live through the builds
    // that only hooked site K: the pad stage resurrected the cleared bytes a
    // few hundred instructions later. Both pad sites sit *before* their own
    // stage's bool→float conversion, so zeroing there also zeroes the
    // fltIsPressed_LMB/RMB/Reload mirrors (+0x274/+0x29C/+0x2A8) that a pad
    // drives through its triggers — those floats are computed from these very
    // bytes by the loop at 0x14273BED7 / 0x14273A742.
    //
    // Register/byte evidence (ACU 1.5.0 dump, `MODLOG/scripts`): all three
    // sites execute with rbx = InputContainerBig* (set at 0x14273BAxx /
    // 0x14273BE24 / 0x142739E57; each function only restores rbx after the
    // site), and the stolen bytes are whole, position-independent
    // instructions the trampoline can re-execute:
    //   0x14273BC7B  6 bytes  48 8B D6 48 8B CB          mov rdx,rsi; mov rcx,rbx
    //   0x14273BEC5  7 bytes  48 8D 93 64 08 00 00       lea rdx,[rbx+0x864]
    //   0x14273A726  8 bytes  4C 8B B4 24 48 01 00 00    mov r14,[rsp+0x148]
    FreeJumpPlugin* g_ParkourModeInstance = nullptr;

    // Per-site hit counters, surfaced in the UI. They answer the one question
    // static analysis cannot: does this site actually execute for the player's
    // device in this build. All-zero while pressing pad buttons means the pad
    // input never reaches these functions, i.e. a different seam is needed.
    uint32_t g_SuppressHitsKbm = 0;
    uint32_t g_SuppressHitsPadKeymap = 0;
    uint32_t g_SuppressHitsMapper = 0;
    uint32_t g_SuppressHitsPadSnapshot = 0;
    uint32_t g_SuppressHitsPadPoll = 0;

    // The pad button code the game's own controller bindings resolve each
    // action to, as last read from the pad keymap. Surfaced in the UI so that
    // "Attack" can be seen to mean, say, pad button 2 on this particular pad.
    int16_t g_PadCodeForAction[kActionKeyCount] = {};

    // Fallback pad-button numbers, used only when the game's own keymap cannot
    // be consulted. The game's pad snapshot lays its digital buttons out at
    // +0x30+code, and the 1.5.0 dump shows the device-6 mapper mapping those
    // slots as 0=A, 1=B, 2=X, 3=Y, 4=RB, 5=LB, which is exactly the player's
    // native layout: Attack=X(2), EagleVision=Y(3), Aim/LB(5), BombDrop/RB(4).
    // -1 means "no known pad button for this action".
    constexpr int16_t kDefaultPadCodeForAction[kActionKeyCount] = {
        -1, // 0x00 ParkourUp            (A - deliberately NOT blocked)
         2, // 0x01 Attack               -> pad X
         3, // 0x02 EagleVision          -> pad Y
        -1, // 0x03 ParkourDownInteract (B - deliberately NOT blocked)
        -1, // 0x04 Disguise
        -1, // 0x05 Numpad4CycleLeftHand
        -1, // 0x06 Medicine
        -1, // 0x07 Numpad6CycleBombs
        -1, // 0x08 Map
        -1, // 0x09 Escape
        -1, // 0x0A Crouch
         5, // 0x0B Aim                 -> pad LB
        -1, // 0x0C Sprint
         4, // 0x0D BombDrop            -> pad RB
        -1, // 0x0E Reload
        -1, // 0x0F CenterCamera
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };

    // Resolved pad button for an action while the mask is applied: the game's
    // own binding when the keymap path has seen it, the native layout above
    // otherwise. Returns -1 only when nothing is known at all.
    int ResolvePadCodeForAction(int actionIdx)
    {
        if (g_PadCodeForAction[actionIdx] >= 0)
            return g_PadCodeForAction[actionIdx];
        return kDefaultPadCodeForAction[actionIdx];
    }

    void ZeroDisabledActionKeyStates(AllRegisters* params)
    {
        FreeJumpPlugin* owner = g_ParkourModeInstance;
        if (!owner)
            return;
        const uint32_t disabledMask = owner->GetDisabledActionMask();
        if (!disabledMask)
            return;
        auto* inputContainer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(params->rbx_));
        if (!inputContainer)
            return;

        uint8_t* keyStatesThisFrame = inputContainer + 0x30; // InputContainer::keyStates_thisFrame
        for (int actionIdx = 0; actionIdx < kActionKeyCount; ++actionIdx)
        {
            if (disabledMask & (1u << actionIdx))
                keyStatesThisFrame[actionIdx] = 0;
        }
    }

    // ── Parkour Mode, gamepad: hide the physical button from the game ──────
    // Zeroing keyStates stops an *action*; it does not stop the game from
    // knowing that pad button 3 is down. The pad stage publishes this frame's
    // snapshot into InputContainerBig+0x864 (0x110 bytes, the same buffer the
    // keymap fill 0x14273B390 reads): button `code` is the byte at +0x30+code
    // with bit 7 as the pressed flag. That buffer is the only pad view the rest
    // of the frame gets, so clearing a button here means the game cannot detect
    // it at all - no action, no press edge, no held-button bookkeeping - while
    // the plugin's own XInput polling (a separate DLL reading the real device)
    // still sees it and can fire parkour triggers bound to that button.
    //
    // WHICH button is decided by the game's own pad keymap, i.e. the player's
    // in-game controller bindings: rdi at this site is that table, entry stride
    // 0x214, entry type at +4, pad button code at +8 (exactly what the fill
    // reads). So "Attack" hides whatever button is bound to attack on this pad
    // instead of a hard-coded layout, and an action bound to a stick axis or a
    // hat compare (entry type 2) is left to the keyStates sites.
    void SanitizePadSnapshotForDisabledActions(AllRegisters* params)
    {
        ++g_SuppressHitsPadSnapshot;
        FreeJumpPlugin* owner = g_ParkourModeInstance;
        if (!owner)
            return;
        const uint32_t disabledMask = owner->GetDisabledActionMask();
        if (!disabledMask)
            return;
        auto* inputContainer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(params->rbx_));
        const auto* padKeymap = reinterpret_cast<const uint8_t*>(
            static_cast<uintptr_t>(params->rdi_));
        if (!inputContainer || !padKeymap)
            return;

        uint8_t* padState = inputContainer + 0x864; // this frame's pad snapshot
        for (int actionIdx = 0; actionIdx < kActionKeyCount; ++actionIdx)
        {
            if (!(disabledMask & (1u << actionIdx)))
                continue;
            // Clear first: an action that resolves to no pad button (axis/hat
            // binding, or not pad-bound at all) must read as unresolved rather
            // than keep last frame's code - a stale value displayed the wrong
            // button next to a ticked action.
            g_PadCodeForAction[actionIdx] = -1;
            const uint8_t* entry = padKeymap + static_cast<size_t>(actionIdx) * 0x214;
            const int32_t entryType = *reinterpret_cast<const int32_t*>(entry + 4);
            const int32_t padCode = *reinterpret_cast<const int32_t*>(entry + 8);
            if (entryType != 1)   // 1 = digital pad button, 2 = axis/hat comparison
                continue;
            if (padCode < 0 || padCode > 0xDE) // buttons live at +0x30 inside 0x110 bytes
                continue;
            g_PadCodeForAction[actionIdx] = static_cast<int16_t>(padCode);
            // Clear the whole byte, not just the pressed bit: a button the game
            // can still see as "held" or as having an edge is still detectable.
            padState[0x30 + padCode] = 0;
        }
    }

    // ── Parkour Mode, gamepad: hide the button at ITS SOURCE ────────────────
    // 0x1427289A0 is the game's pad poll, and it is the ONE function both
    // gamepad paths go through:
    //   0x14273BEA1  call 0x1427289A0   device id 9  -> writes InputContainerBig+0x864
    //   0x14273EA6   call 0x1427289A0   every other id -> writes a stack copy at rsp+0x20
    // Both are a 0x110-byte DirectInput state whose digital buttons live at
    // +0x30+code (bit 7 = pressed). Inside the poll the destination buffer is
    // rsi (set at 0x142728B8 from the caller's rdx) and rbx is the
    // InputContainerBig*.
    //
    // Hooking at 0x142728A1F - the instruction right after GetDeviceState
    // returned and filled the buffer - therefore lets one site clear the
    // buttons for BOTH paths, before the keymap fill (0x14273B390) or the
    // per-device mapper (0x142739E40) can turn them into actions. The game is
    // never handed the button at all: no action, no press edge, no hold.
    //
    // The rest of the poll function only reads the container's own analog
    // fields (+0x380) and the stack scratch area, never the buffer we clear,
    // so the force-feedback / vibration tail still works; and only the digital
    // button bytes are touched, so the sticks stay fully alive.
    void SanitizePadPollSnapshot(AllRegisters* params)
    {
        ++g_SuppressHitsPadPoll;
        FreeJumpPlugin* owner = g_ParkourModeInstance;
        if (!owner)
            return;
        const uint32_t disabledMask = owner->GetDisabledActionMask();
        if (!disabledMask)
            return;
        auto* padState = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(params->rsi_));
        if (!padState)
            return;
        for (int actionIdx = 0; actionIdx < kActionKeyCount; ++actionIdx)
        {
            if (!(disabledMask & (1u << actionIdx)))
                continue;
            const int padCode = ResolvePadCodeForAction(actionIdx);
            if (padCode < 0)
                continue;
            // Clear the whole byte, not just bit 7, so the button is not merely
            // unpressed but indistinguishable from "not touched this frame".
            padState[0x30 + padCode] = 0;
        }
    }

    void SuppressDisabledActionKeys_Kbm(AllRegisters* params)
    {
        ++g_SuppressHitsKbm;
        ZeroDisabledActionKeyStates(params);
    }

    void SuppressDisabledActionKeys_PadKeymap(AllRegisters* params)
    {
        ++g_SuppressHitsPadKeymap;
        ZeroDisabledActionKeyStates(params);
    }

    void SuppressDisabledActionKeys_Mapper(AllRegisters* params)
    {
        ++g_SuppressHitsMapper;
        ZeroDisabledActionKeyStates(params);
    }

    // One holder per site: a plugin that owns one of them must not cost us the
    // other two devices.
    struct ParkourModeKbmSuppressHook : AutoAssemblerCodeHolder_Base
    {
        ParkourModeKbmSuppressHook()
        {
            PresetScript_CCodeInTheMiddle(
                0x14273BC7B, 6,
                SuppressDisabledActionKeys_Kbm,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                true);
        }
    };

    struct ParkourModePadKeymapSuppressHook : AutoAssemblerCodeHolder_Base
    {
        ParkourModePadKeymapSuppressHook()
        {
            PresetScript_CCodeInTheMiddle(
                0x14273BEC5, 7,
                SuppressDisabledActionKeys_PadKeymap,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                true);
        }
    };

    struct ParkourModeMapperSuppressHook : AutoAssemblerCodeHolder_Base
    {
        ParkourModeMapperSuppressHook()
        {
            PresetScript_CCodeInTheMiddle(
                0x14273A726, 8,
                SuppressDisabledActionKeys_Mapper,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                true);
        }
    };

    AutoAssembleWrapper<ParkourModeKbmSuppressHook>& GetParkourModeKbmSuppressHook()
    {
        static AutoAssembleWrapper<ParkourModeKbmSuppressHook> s_hook;
        return s_hook;
    }

    AutoAssembleWrapper<ParkourModePadKeymapSuppressHook>& GetParkourModePadKeymapSuppressHook()
    {
        static AutoAssembleWrapper<ParkourModePadKeymapSuppressHook> s_hook;
        return s_hook;
    }

    AutoAssembleWrapper<ParkourModeMapperSuppressHook>& GetParkourModeMapperSuppressHook()
    {
        static AutoAssembleWrapper<ParkourModeMapperSuppressHook> s_hook;
        return s_hook;
    }

    // Hides the disabled actions' pad buttons in the pad snapshot itself,
    // before the game's keymap fill reads it (device-9 / keymap pad path).
    // Vanilla 0x14273BEB3 is `lea rdx,[rbx+0x864]` - 7 whole bytes, and it runs
    // after the poll at 0x14273BEA1 filled the snapshot and before the fill
    // call at 0x14273BEC0 consumes it. rbx is InputContainerBig*, rdi the pad
    // keymap; the site after the fill (0x14273BEC5) is a separate hook.
    struct ParkourModePadSnapshotHook : AutoAssemblerCodeHolder_Base
    {
        ParkourModePadSnapshotHook()
        {
            PresetScript_CCodeInTheMiddle(
                0x14273BEB3, 7,
                SanitizePadSnapshotForDisabledActions,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                true);
        }
    };

    AutoAssembleWrapper<ParkourModePadSnapshotHook>& GetParkourModePadSnapshotHook()
    {
        static AutoAssembleWrapper<ParkourModePadSnapshotHook> s_hook;
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
    // the two suppression sites above (watch the site-hit counters: all zero
    // while pressing pad buttons means this fallback is the only option left).
    // It disables the ENTIRE controller, sticks included, so the character
    // cannot move while Parkour Mode is on — which is why it is now off by
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

    // Each suppression site is asserted separately so one owned by another
    // plugin does not disable the other two.
    bool IsKbmSuppressSitePatched()
    {
        // Vanilla: mov rdx,rsi ; mov rcx,rbx  (0x14273BC7B, 6 bytes).
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x14273BC7B);
        return !(site[0] == 0x48 && site[1] == 0x8B && site[2] == 0xD6
            && site[3] == 0x48 && site[4] == 0x8B && site[5] == 0xCB);
    }

    bool IsPadKeymapSuppressSitePatched()
    {
        // Vanilla: lea rdx,[rbx+0x864]  (0x14273BEC5, 7 bytes).
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x14273BEC5);
        return !(site[0] == 0x48 && site[1] == 0x8D && site[2] == 0x93
            && site[3] == 0x64 && site[4] == 0x08 && site[5] == 0x00 && site[6] == 0x00);
    }

    bool IsPadSnapshotSitePatched()
    {
        // Vanilla: lea rdx,[rbx+0x864]  (0x14273BEB3, 7 bytes).
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x14273BEB3);
        return !(site[0] == 0x48 && site[1] == 0x8D && site[2] == 0x93
            && site[3] == 0x64 && site[4] == 0x08 && site[5] == 0x00 && site[6] == 0x00);
    }

    bool IsPadPollSitePatched()
    {
        // Vanilla: movzx edi,dil ; test eax,eax  (0x142728A1F, 6 bytes).
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x142728A1F);
        return !(site[0] == 0x40 && site[1] == 0x0F && site[2] == 0xB6 && site[3] == 0xFF
            && site[4] == 0x85 && site[5] == 0xC0);
    }

    bool IsMapperSuppressSitePatched()
    {
        // Vanilla: mov r14,[rsp+0x148]  (0x14273A726, 8 bytes).
        const uint8_t* site = reinterpret_cast<const uint8_t*>(0x14273A726);
        return !(site[0] == 0x4C && site[1] == 0x8B && site[2] == 0xB4 && site[3] == 0x24
            && site[4] == 0x48 && site[5] == 0x01 && site[6] == 0x00 && site[7] == 0x00);
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

void FreeJumpPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;

    std::vector<ParkourActionOverride> loadedOverrides;
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
        else if (TryGetValue(trimmedLine, "SnapDirectionKey=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidBindableKey(keyCode))
                m_SnapDirectionKey = keyCode;
        }
        else if (TryGetValue(trimmedLine, "SnapDirectionKeyPad=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidBindableKey(keyCode))
                m_SnapDirectionKeyPad = keyCode;
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
        // Legacy `BlockPadWhileParkour=` is deliberately NOT read: in 0.11 it
        // defaulted to 1 and was the primary (whole-pad) mechanism. Honouring a
        // stored 1 would silently re-enable the detach as the active fix and
        // freeze the sticks, hiding the per-site suppression. Unknown keys are
        // skipped, so old files load cleanly and the option is simply off.
        else if (TryGetValue(trimmedLine, "ParkourDisabledActions=", value))
        {
            uint32_t mask = 0;
            for (const std::string_view actionToken : Split(value, '|'))
            {
                int actionIdx = 0;
                if (TryParseInteger(actionToken, 10, actionIdx)
                    && actionIdx >= 0 && actionIdx < kActionKeyCount)
                {
                    mask |= (1u << actionIdx);
                }
            }
            m_DisabledActionMask = mask;
        }
        else if (TryGetValue(trimmedLine, "SnapDirectionMode=", value))
        {
            bool enabled = false;
            if (TryParseBool(value, enabled))
                m_SnapDirectionMode = enabled;
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
    file << "SnapDirectionKey=" << std::hex << m_SnapDirectionKey << std::dec << "\n";
    file << "SnapDirectionKeyPad=" << std::hex << m_SnapDirectionKeyPad << std::dec << "\n";
    file << "SnapDirectionMode=" << (m_SnapDirectionMode ? 1 : 0) << "\n";
    file << "UseControllerBindings=" << (m_UseControllerBindings ? 1 : 0) << "\n";
    file << "ParkourModeKey=" << std::hex << m_ParkourModeKey << std::dec << "\n";
    file << "ParkourModeKeyPad=" << std::hex << m_ParkourModeKeyPad << std::dec << "\n";
    file << "ParkourModeOn=" << (m_ParkourModeOn ? 1 : 0) << "\n";
    file << "DetachPadFallback=" << (m_BlockPadWhileParkour ? 1 : 0) << "\n";
    file << "ParkourDisabledActions=";
    {
        bool firstAction = true;
        for (int actionIdx = 0; actionIdx < kActionKeyCount; ++actionIdx)
        {
            if (!(m_DisabledActionMask & (1u << actionIdx)))
                continue;
            if (!firstAction)
                file << "|";
            file << actionIdx;
            firstAction = false;
        }
    }
    file << "\n";
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
    m_Callbacks.ChooseAfterSorting_fnp = nullptr;
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 5.0f;
    m_Callbacks.m_Name = "FreeJumpPlugin";

    if (m_ParkourModeOn && m_DisabledActionMask)
        ApplyParkourMode(true);
}

void FreeJumpPlugin::OnBeforeDeactivate()
{
    if (m_KbmSuppressActive || m_PadKeymapSuppressActive || m_MapperSuppressActive
        || m_PadSnapshotSuppressActive || m_PadPollSanitizeActive || m_PadBlockHookActive)
        ApplyParkourMode(false);
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
        // Every suppression callback reads this, so it has to be set before any
        // hook below can fire - including when the KBM site is owned by
        // someone else and only a pad site installs.
        g_ParkourModeInstance = this;

        if (!m_KbmSuppressActive && !m_PadKeymapSuppressActive && !m_MapperSuppressActive
            && !m_PadSnapshotSuppressActive)
        {
            // Fresh activation: the counters then describe this ON period.
            g_SuppressHitsKbm = 0;
            g_SuppressHitsPadKeymap = 0;
            g_SuppressHitsMapper = 0;
            g_SuppressHitsPadSnapshot = 0;
            g_SuppressHitsPadPoll = 0;
            for (int16_t& padCode : g_PadCodeForAction)
                padCode = -1;
        }

        // Three independent sites. One that another plugin already patched is
        // skipped (never fought over) and costs only its own device; the UI
        // reports which sites are live.
        bool anyRefused = false;
        if (!m_KbmSuppressActive)
        {
            if (IsKbmSuppressSitePatched())
                anyRefused = true;
            else
            {
                GetParkourModeKbmSuppressHook().Activate();
                m_KbmSuppressActive = true;
            }
        }
        if (!m_PadKeymapSuppressActive)
        {
            if (IsPadKeymapSuppressSitePatched())
                anyRefused = true;
            else
            {
                GetParkourModePadKeymapSuppressHook().Activate();
                m_PadKeymapSuppressActive = true;
            }
        }
        if (!m_MapperSuppressActive)
        {
            if (IsMapperSuppressSitePatched())
                anyRefused = true;
            else
            {
                GetParkourModeMapperSuppressHook().Activate();
                m_MapperSuppressActive = true;
            }
        }
        // Hides the buttons themselves. Independent of the three above: this is
        // the site that makes the game unable to detect the button rather than
        // just ignoring the action it drives.
        if (!m_PadSnapshotSuppressActive)
        {
            if (IsPadSnapshotSitePatched())
                anyRefused = true;
            else
            {
                GetParkourModePadSnapshotHook().Activate();
                m_PadSnapshotSuppressActive = true;
            }
        }
        // The primary gamepad mechanism: the button never reaches the game on
        // either pad path (see ParkourModePadPollHook). Installed last so that,
        // even if every keyStates site is taken, the buttons stay hidden.
        if (!m_PadPollSanitizeActive)
        {
            if (IsPadPollSitePatched())
            {
                anyRefused = true;
            }
            else
            {
                GetParkourModePadPollHook().Activate();
                m_PadPollSanitizeActive = true;
            }
        }
        m_ParkourHookRefused = anyRefused;

        // Optional fallback, off by default: detach the pad device. It covers
        // every device id without a per-button mapping, but it also disables
        // the sticks. See ParkourModePadBlockHook.
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
        if (m_KbmSuppressActive)
        {
            GetParkourModeKbmSuppressHook().Deactivate();
            m_KbmSuppressActive = false;
        }
        if (m_PadKeymapSuppressActive)
        {
            GetParkourModePadKeymapSuppressHook().Deactivate();
            m_PadKeymapSuppressActive = false;
        }
        if (m_MapperSuppressActive)
        {
            GetParkourModeMapperSuppressHook().Deactivate();
            m_MapperSuppressActive = false;
        }
        if (m_PadSnapshotSuppressActive)
        {
            GetParkourModePadSnapshotHook().Deactivate();
            m_PadSnapshotSuppressActive = false;
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

    // Snap Direction: candidates pointing away from the held input are removed
    // instead of having their fitness zeroed (mirrors ChooseBeforeFiltering).
    if (m_SnapDirectionMode)
    {
        const Vector2f camForward = GetCameraFlatForward();
        const float camYaw = atan2f(camForward.x, camForward.y);
        float inputX = 0.0f, inputY = 0.0f;
        if (GetAsyncKeyState(0x57) & 0x8000) inputY += 1.0f;
        if (GetAsyncKeyState(0x53) & 0x8000) inputY -= 1.0f;
        if (GetAsyncKeyState(0x41) & 0x8000) inputX -= 1.0f;
        if (GetAsyncKeyState(0x44) & 0x8000) inputX += 1.0f;
        Vector2f inputDir(inputX, inputY);
        const float len = sqrtf(inputDir.x * inputDir.x + inputDir.y * inputDir.y);
        if (len > 0.001f)
        {
            inputDir.x /= len;
            inputDir.y /= len;
            Rotate2D(inputDir.x, inputDir.y, camYaw);
            Vector4f* dirPtr = (Vector4f*)((uintptr_t)action + 0x70);
            const float dLen = sqrtf(dirPtr->x * dirPtr->x + dirPtr->y * dirPtr->y);
            if (dLen >= 0.001f)
            {
                const float dot = (dirPtr->x / dLen) * inputDir.x + (dirPtr->y / dLen) * inputDir.y;
                if (dot < 0.0f)
                    return true;
            }
        }
    }

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
    UpdateKeyState(m_SnapDirectionKey);
    UpdateKeyState(m_SnapDirectionKeyPad);
    // Without this refresh, m_KeyPressedThisFrame[m_ParkourModeKey] kept its
    // last value from whenever a rebind-wait last ran (it is only cleared by
    // UpdateKeyState). After the first rebind it stayed true forever, so
    // WasKeyPressed fired on every frame and Parkour Mode flipped ON/OFF each
    // frame — rewriting the INI and installing/removing the suppression hooks
    // at frame rate.
    UpdateKeyState(m_ParkourModeKey);
    UpdateKeyState(m_ParkourModeKeyPad);
    for (const ParkourActionOverride& override : m_Overrides)
    {
        UpdateKeyState(override.keyCode);
        UpdateKeyState(override.padKeyCode);
    }

    const bool waitingForOverrideKey = std::any_of(
        m_Overrides.begin(),
        m_Overrides.end(),
        [](const ParkourActionOverride& override) { return override.waitingForKey; });
    const bool anyRebindWaiting = m_WaitingForKey || m_WaitingForSnapKey
        || m_WaitingForParkourKey || waitingForOverrideKey;
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
    else if (m_WaitingForSnapKey)
    {
        if (routeFeatureKey(m_SnapDirectionKey, m_SnapDirectionKeyPad))
        {
            m_JustCapturedKey = m_UseControllerBindings ? m_SnapDirectionKeyPad : m_SnapDirectionKey;
            m_WaitingForSnapKey = false;
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

    const int effectiveSnapKey = EffectiveSnapDirectionKey();
    if (WasKeyPressed(effectiveSnapKey) && effectiveSnapKey != m_JustCapturedKey
        && !MouseBindWouldHitUi(effectiveSnapKey))
    {
        m_SnapDirectionMode = !m_SnapDirectionMode;
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

    const bool wantParkourHook = m_ParkourModeOn && m_DisabledActionMask != 0;
    const bool anyParkourSuppressActive =
        m_KbmSuppressActive || m_PadKeymapSuppressActive || m_MapperSuppressActive
        || m_PadSnapshotSuppressActive || m_PadPollSanitizeActive;
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

    const bool needHook = freeJumpHeld || anyOverrideActive || m_SnapDirectionMode;

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
    bool freeMode   = IsKeyHeld(m_ToggleKey);
    bool snapMode   = m_SnapDirectionMode;
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
    const ImVec4 holdColor(0.20f, 0.70f, 1.00f, 1.00f);
    const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);

    ImGui::Text("FreeJumpPlugin");
    if (m_HookRefused)
    {
        ImGui::TextWrapped(
            "Parkour sort/select (0x140133B00) is owned by another plugin (for example ACUFixes' "
            "Parkour Helpers, or ACU_ParkourPatches). The features below still work through a "
            "compatibility hook on the game's own action filter, so both plugins stay enabled.");
        if (m_OverrideFallbackRefused)
        {
            ImGui::TextWrapped(
                "The compatibility hook site (0x1401D4DE0) is also patched by another plugin, so "
                "overrides cannot be applied. Disable one of the conflicting plugins to restore "
                "them.");
        }
        else if (m_OverrideFallbackActive)
        {
            ImGui::TextDisabled(
                "Compatibility mode: Free Jump, Snap Direction, and every override work with "
                "'Remove all other actions'; the per-override fitness multiplier is not applied "
                "in this mode.");
        }
    }
    if (m_ParkourHookRefused)
    {
        ImGui::TextWrapped(
            "One of Parkour Mode's input sites is owned by another plugin (keyboard/mouse "
            "0x14273BC7B, gamepad keymap 0x14273BEC5, gamepad mapper 0x14273A726). The other "
            "sites stay installed, so only that device's suppression is missing; the line "
            "under the Parkour Mode toggle shows which sites are live.");
    }
    if (m_PadDllLoaded)
    {
        if (m_ActivePadSlot >= 0)
            ImGui::Text("Gamepad: connected (slot %d)", m_ActivePadSlot);
        else
            ImGui::Text("Gamepad: none detected");
    }
    else
    {
        ImGui::TextDisabled("Gamepad: XInput unavailable on this system");
    }
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
        if (m_UseControllerBindings)
            ImGui::TextDisabled("(profiles: CONTROLLER — keyboard/mouse keys ignored)");
        else
            ImGui::TextDisabled("(profiles: KEYBOARD/MOUSE — controller keys ignored)");
        ImGui::TextDisabled(
            "Both device layouts are stored side by side; with this unchecked you rebind with "
            "your keyboard/mouse, and with it checked you rebind with your controller. Everything "
            "else in this menu behaves the same in either mode.");

        ImGui::Separator();
        ImGui::Text("Free Jump:");
        ImGui::SameLine();
        ImGui::TextColored(freeMode ? activeColor : inactiveColor, freeMode ? "ACTIVE (HOLD)" : "OFF");
        if (m_UseControllerBindings)
            ImGui::Text("  Bound key: %s", KeyCodeLabel(m_ToggleKeyPad).c_str());
        else
            ImGui::Text("  Bound key: %s", KeyCodeLabel(m_ToggleKey).c_str());
        if (m_WaitingForKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Input key to bind..."); }
        if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind##FreeJump"))
        {
            m_WaitingForKey = !m_WaitingForKey;
            if (m_WaitingForKey)
            {
                m_WaitingForSnapKey = false;
                m_WaitingForParkourKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
            }
        }

        ImGui::Separator();
        ImGui::Text("Snap Direction:");
        ImGui::SameLine();
        ImGui::TextColored(snapMode ? ImVec4(0.20f,0.60f,0.90f,1.0f) : ImVec4(0.70f,0.70f,0.70f,1.0f),
                           snapMode ? "ON" : "OFF");
        ImGui::Text("  Toggle key: %s",
            KeyCodeLabel(m_UseControllerBindings ? m_SnapDirectionKeyPad : m_SnapDirectionKey).c_str());
        if (m_WaitingForSnapKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Input key to bind..."); }
        if (ImGui::Button(m_WaitingForSnapKey ? "Cancel Rebind" : "Rebind##Snap"))
        {
            m_WaitingForSnapKey = !m_WaitingForSnapKey;
            if (m_WaitingForSnapKey)
            {
                m_WaitingForKey = false;
                m_WaitingForParkourKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
            }
        }

        ImGui::Separator();
        ImGui::Text("Parkour Mode:");
        ImGui::SameLine();
        ImGui::TextColored(m_ParkourModeOn ? activeColor : inactiveColor,
                           m_ParkourModeOn ? "ON" : "OFF");
        if (m_ParkourModeOn)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(frees the disabled actions' keys below)");
        }
        ImGui::Text("  Toggle key: %s",
            KeyCodeLabel(m_UseControllerBindings ? m_ParkourModeKeyPad : m_ParkourModeKey).c_str());
        // Live diagnostics: which suppression sites are installed, and whether
        // each one actually executes this session. "pad snapshot" is the one
        // that makes the game unable to detect the button; the other two only
        // clear the action state. If the pad sites show "on" but their hit
        // count stays 0 while pad buttons are pressed, this build routes
        // gamepad input somewhere else - only then is the detach fallback
        // below worth using.
        ImGui::TextDisabled(
            "Sites: KBM %s | pad poll %s | pad keymap %s | pad mapper %s | pad snapshot %s",
            m_KbmSuppressActive ? "on" : (IsKbmSuppressSitePatched() ? "taken" : "off"),
            m_PadPollSanitizeActive ? "on" : (IsPadPollSitePatched() ? "taken" : "off"),
            m_PadKeymapSuppressActive ? "on" : (IsPadKeymapSuppressSitePatched() ? "taken" : "off"),
            m_MapperSuppressActive ? "on" : (IsMapperSuppressSitePatched() ? "taken" : "off"),
            m_PadSnapshotSuppressActive ? "on" : (IsPadSnapshotSitePatched() ? "taken" : "off"));
        ImGui::TextDisabled(
            "Hits since ON: KBM %u | pad poll %u | pad keymap %u | pad mapper %u | pad snapshot %u   (disabled mask 0x%02X)",
            (unsigned)g_SuppressHitsKbm, (unsigned)g_SuppressHitsPadPoll,
            (unsigned)g_SuppressHitsPadKeymap,
            (unsigned)g_SuppressHitsMapper, (unsigned)g_SuppressHitsPadSnapshot,
            (unsigned)m_DisabledActionMask);
        // Which physical pad button each disabled action hides, as resolved from
        // the game's own controller bindings. This is the line to read when a
        // button still reaches the game: if a ticked action is missing here, it
        // is not bound to a pad button on this device.
        if ((m_PadSnapshotSuppressActive && g_SuppressHitsPadSnapshot)
            || (m_PadPollSanitizeActive && g_SuppressHitsPadPoll))
        {
            std::string hiddenButtons;
            for (int actionIdx = 0; actionIdx < kActionKeyCount; ++actionIdx)
            {
                if (!(m_DisabledActionMask & (1u << actionIdx)))
                    continue;
                const int padCode = ResolvePadCodeForAction(actionIdx);
                if (padCode < 0)
                    continue;
                const bool fromKeymap = g_PadCodeForAction[actionIdx] >= 0;
                if (!hiddenButtons.empty())
                    hiddenButtons += ", ";
                hiddenButtons += ActionKeyName(actionIdx);
                hiddenButtons += " = pad button ";
                hiddenButtons += std::to_string(padCode);
                if (!fromKeymap)
                    hiddenButtons += " (native layout)";
            }
            if (hiddenButtons.empty())
                ImGui::TextDisabled("Pad buttons hidden: none resolved (no ticked action is pad-bound)");
            else
                ImGui::TextDisabled("Pad buttons hidden: %s", hiddenButtons.c_str());
        }
        bool blockPad = m_BlockPadWhileParkour;
        if (ImGui::Checkbox("Fallback: detach the controller (also stops stick movement)", &blockPad))
        {
            m_BlockPadWhileParkour = blockPad;
            SaveSettings();
            if (m_ParkourModeOn)
                ApplyParkourMode(false), ApplyParkourMode(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Off by default, and normally not needed: the two gamepad sites above free the "
                "disabled actions per button while the sticks keep working. This option instead "
                "returns 0 from the game's own pad poll, so the game ignores the whole controller "
                "- buttons and sticks - and the character cannot move while Parkour Mode is on. "
                "Only use it if the pad hit counters stay at 0 while you press pad buttons.");
        if (m_WaitingForParkourKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Input key or pad button to bind..."); }
        if (ImGui::Button(m_WaitingForParkourKey ? "Cancel Rebind##ParkourMode" : "Rebind##ParkourMode"))
        {
            m_WaitingForParkourKey = !m_WaitingForParkourKey;
            if (m_WaitingForParkourKey)
            {
                m_WaitingForKey = false;
                m_WaitingForSnapKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
            }
        }
        if (ImGui::TreeNode("Actions disabled while Parkour Mode is on"))
        {
            ImGui::TextWrapped(
                "While Parkour Mode is active these ActionKeyCode entries are forced unpressed "
                "(keyboard, mouse, and gamepad bindings), so their buttons are free for override "
                "bindings. Your keybindings are never modified, and everything returns to normal "
                "when the mode is switched off.");
            ImGui::TextWrapped(
                "On a controller, ticking an action also hides the button the game has it bound "
                "to: the pad snapshot the game reads is cleared for that button, so the game "
                "cannot detect it at all (no action, no press, no hold). The plugin still reads "
                "the real pad, so a parkour trigger bound to that button works. Ticking Attack "
                "therefore hides pad X, EagleVision hides pad Y, Aim hides LB and BombDrop hides "
                "RB if those are your controller bindings - the line under the toggle shows what "
                "each ticked action actually resolved to on your pad.");
            bool maskChanged = false;
            for (int actionIdx = 0; actionIdx < kActionKeyCount; ++actionIdx)
            {
                if (!*ActionKeyName(actionIdx))
                    continue;
                bool disabled = (m_DisabledActionMask & (1u << actionIdx)) != 0;
                if (ImGui::Checkbox(ActionKeyName(actionIdx), &disabled))
                {
                    m_DisabledActionMask = disabled
                        ? (m_DisabledActionMask | (1u << actionIdx))
                        : (m_DisabledActionMask & ~(1u << actionIdx));
                    maskChanged = true;
                }
            }
            if (ImGui::Button("Restore defaults (X / Y / LB / Crouch)"))
            {
                // Attack=pad X, EagleVision=pad Y, Aim=pad LB (+Quickshot, which is
                // Aim held while a target is locked), Crouch=the pad's analog
                // trigger. This is the full list the player asked to be freed on a
                // controller, and it is also the constructor default.
                m_DisabledActionMask = (1u << 1) | (1u << 2) | (1u << 11) | (1u << 10);
                maskChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Select controller actions (X / Y / LB / RB)"))
            {
                // The four native controller bindings: Attack = pad X,
                // EagleVision = pad Y, Aim/quick-shot = LB, BombDrop = RB.
                // Additive, so a click never silently re-enables something the
                // player deliberately ticked off. Crouch stays in the mask, and
                // on a controller it is driven by an analog trigger rather than
                // a digital button, which is exactly what the keyStates sites
                // (pad keymap / pad mapper) are for.
                m_DisabledActionMask |= (1u << 1) | (1u << 2) | (1u << 11) | (1u << 13);
                maskChanged = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Ticks Attack (pad X), EagleVision (pad Y), Aim (LB) and BombDrop (RB) - "
                    "the four native controller bindings - and leaves everything else as it is. "
                    "While Parkour Mode is on the game cannot detect those buttons at all; the "
                    "plugin still reads them, so an override bound to X, Y, LB or RB still fires. "
                    "Quickshot is Aim held while a target is locked, so it has no key of its own "
                    "and is freed by the same tick. When the pad snapshot site below reports "
                    "'on', the line under the toggle lists which pad button each ticked action "
                    "resolved to.");
            if (maskChanged)
                SaveSettings();
            ImGui::TreePop();
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
            m_WaitingForSnapKey = false;
            m_WaitingForParkourKey = false;
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
                    // Editable name (inline, right after the status label).
                    // The buffer is per-render so a never-edited name still shows
                    // the current value, and Enter commits the edit.
                    {
                        ImGui::SetNextItemWidth(220.0f);
                        static std::string nameBuffer;
                        nameBuffer = ov.name;
                        if (ImGui::InputText("##OverrideName", nameBuffer.data(), nameBuffer.size() + 1,
                                ImGuiInputTextFlags_EnterReturnsTrue))
                        {
                            std::string edited(nameBuffer.c_str());
                            std::replace(edited.begin(), edited.end(), ',', ';');
                            if (ov.name != edited)
                            {
                                ov.name = edited;
                                SaveSettings();
                            }
                        }
                        ImGui::SameLine();
                    }
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
                            m_WaitingForSnapKey = false;
                            m_WaitingForParkourKey = false;
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
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "While this override is active, matching parkour actions are given a much "
                            "more permissive acceptance range (only widened, never narrowed).");
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

    // Snap Direction
    if (m_SnapDirectionMode)
    {
        Vector2f camForward = GetCameraFlatForward();
        float camYaw = atan2f(camForward.x, camForward.y);
        float inputX = 0.0f, inputY = 0.0f;
        if (GetAsyncKeyState(0x57) & 0x8000) inputY += 1.0f;
        if (GetAsyncKeyState(0x53) & 0x8000) inputY -= 1.0f;
        if (GetAsyncKeyState(0x41) & 0x8000) inputX -= 1.0f;
        if (GetAsyncKeyState(0x44) & 0x8000) inputX += 1.0f;
        Vector2f inputDir(inputX, inputY);
        float len = sqrtf(inputDir.x * inputDir.x + inputDir.y * inputDir.y);
        if (len > 0.001f)
        {
            inputDir.x /= len;
            inputDir.y /= len;
            Rotate2D(inputDir.x, inputDir.y, camYaw);
            for (int i = 0; i < actions.size; ++i)
            {
                AvailableParkourAction* action = actions[i];
                if (!action) continue;
                Vector4f* dirPtr = (Vector4f*)((uintptr_t)action + 0x70);
                float dx = dirPtr->x, dy = dirPtr->y;
                float dLen = sqrtf(dx*dx + dy*dy);
                if (dLen < 0.001f) continue;
                float dot = (dx / dLen) * inputDir.x + (dy / dLen) * inputDir.y;
                if (dot < 0.0f)
                    *(float*)((uintptr_t)action + 0x204) = 0.0f;
            }
        }
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
