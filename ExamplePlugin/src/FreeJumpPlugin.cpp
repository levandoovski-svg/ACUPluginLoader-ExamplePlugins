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

// Pad analog of the mouse-vector update (0x14273C190): reads the gamepad
// keymap's axis entries plus the pad state buffer at InputContainerBig+0x864
// and writes the pad wasd/mouse vectors. Reproduced by the Parkour Mode
// gamepad-site callback (its rel32 call is stolen, not re-executed).
DEFINE_GAME_FUNCTION(PadActionAxisAndVectorUpdate, 0x14273C4C0, void, __fastcall, (void* inputContainerBig, void* padStateBuffer, void* padKeymap));

// Radial-deadzone/normalization helper for the input vectors (takes the
// InputContainerBig* in rcx; only parameter it reads). Called at the tail of
// the gamepad hard-mapper right before its bool→float conversion — reproduced
// by Parkour Mode's third suppression callback.
DEFINE_GAME_FUNCTION(InputVectorDeadzoneApply, 0x142735E20, void, __fastcall, (void* inputContainerBig, void* a2, void* a3, void* a4));

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
        if (fields.size() != 8 && fields.size() != 10)
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
        if (fields.size() == 10)
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

    // ── Parkour mode: action-key suppression ──────────────────────
    // The per-frame input-update chain (vfunc slot 0x0C of the keymap owner)
    // fills InputContainerBig::keyStates_thisFrame (+0x30) from two device
    // keymaps, and Parkour Mode clears disabled entries after BOTH fills:
    //
    // Site 1 (keyboard/mouse), 0x14273BC7B: inside the chain section that runs
    // after the KBM keymap fill (0x14273B2B0) and before the mouse-vector
    // update (0x14273C190). rbx_ is the InputContainerBig*. The 6 stolen
    // bytes (48 8B D6 48 8B CB = mov rdx,rsi; mov rcx,rbx) are whole
    // instructions re-executed by the trampoline. Verified against the 1.5.0
    // runtime dump; chains behind the ACUFixes-0.9.3 mousewheel-cycle hook at
    // 0x14273BC75, whose trampoline returns exactly to 0x14273BC7B.
    //
    // Site 2 (gamepad keymap stage), 0x14273BED2: the chain continues into a
    // pad-specific stage (entered at 0x14273BE10, which only proceeds when the
    // active input device id is 9: cmp dword ptr [InputContainerBig+0x830],9).
    // There FUN_14273BE6D polls the pad into InputContainerBig+0x864 and
    // FUN_14273b390 re-fills ALL 32 keyStates_thisFrame entries from the pad
    // keymap (rdi) — AFTER site 1 — so pad presses would resurrect suppressed
    // actions. Site 2 sits between that pad fill and the per-action
    // bool→float conversion; the stolen instruction is the whole rel32 call
    // `call FUN_14273c4c0` (vanilla bytes E8 E9 05 00 00, pad analog of the
    // mouse-vector update). A rel32 call cannot be re-executed verbatim from
    // the trampoline, so the callback reproduces it from the saved registers
    // (rcx=InputContainerBig*, rdx=+0x864 pad state, r8=pad keymap — all set
    // by the preceding instructions) and THEN clears the disabled entries, so
    // the following conversion (and therefore fltIsPressed_* and every
    // downstream consumer) sees zeros.
    //
    // Site 3 (gamepad hard-mapped stage), 0x14273A721: the game does NOT use
    // the pad keymap for most gamepads. For every device id other than 9 the
    // chain jumps from 0x14273BE68 into a hard-coded per-device-type mapper
    // (0x142739E40..0x14273A736) whose switch over the active device id
    // (cases 0-7, 10, 0xB — DirectInput/HID device layer; the game imports no
    // XInputGetState at all, so DS4Windows pads arrive here too) writes pad
    // buttons DIRECTLY into keyStates[0..15] (+0x30..+0x3F) and the sticks
    // into the wasd/camera vectors — again AFTER site 1. Site 3 sits at that
    // mapper's tail: the whole rel32 `call FUN_142735e20` (vanilla bytes
    // E8 FA B6 FF FF, radial-deadzone helper taking the container in rcx —
    // its only read parameter), immediately before the mapper's own
    // bool→float conversion. The callback reproduces the call from the saved
    // rcx and then clears the disabled entries. The mapper is skipped for
    // keyboard/mouse devices ([container+0x340] gate) and when the device
    // poll fails, so site 3 only executes while a gamepad drives input.
    // Dump evidence: MODLOG/scripts/pad_input_out.txt, pad_chain_out.txt,
    // pad_chain2_out.txt, pad_poll_out.txt, helper_out.txt. All three sites'
    // vanilla bytes are asserted before install.
    FreeJumpPlugin* g_ParkourModeInstance = nullptr;

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

    void SuppressDisabledActionKeys(AllRegisters* params)
    {
        ZeroDisabledActionKeyStates(params);
    }

    void SuppressDisabledActionKeysAfterPadFill(AllRegisters* params)
    {
        // Reproduce the stolen `call PadActionAxisAndVectorUpdate` first, with
        // the exact register state the original call site had, so the
        // pad-vector update sees its original inputs. AllRegisters is only
        // written back after the callback returns, and the code that resumes
        // at 0x14273BED7 reloads every register it uses before reading one,
        // so the call's register clobbers do not leak.
        PadActionAxisAndVectorUpdate(
            reinterpret_cast<void*>(static_cast<uintptr_t>(params->rcx_)),
            reinterpret_cast<void*>(static_cast<uintptr_t>(params->rdx_)),
            reinterpret_cast<void*>(static_cast<uintptr_t>(params->r8_)));

        ZeroDisabledActionKeyStates(params);
    }

    void SuppressDisabledActionKeysAfterHardmappedPad(AllRegisters* params)
    {
        // Reproduce the stolen `call InputVectorDeadzoneApply` (container in
        // rcx; the extra saved registers are passed for exactness although the
        // helper only reads rcx), then clear the disabled entries so the
        // mapper's bool→float conversion at 0x14273A736 sees zeros. Code
        // resuming at 0x14273A726 reloads R14/RSI from the stack and rebinds
        // RAX/RCX/EDX before use, so register clobbers do not leak.
        InputVectorDeadzoneApply(
            reinterpret_cast<void*>(static_cast<uintptr_t>(params->rcx_)),
            reinterpret_cast<void*>(static_cast<uintptr_t>(params->rdx_)),
            reinterpret_cast<void*>(static_cast<uintptr_t>(params->r8_)),
            reinterpret_cast<void*>(static_cast<uintptr_t>(params->r9_)));

        ZeroDisabledActionKeyStates(params);
    }

    struct ParkourModeSuppressHook : AutoAssemblerCodeHolder_Base
    {
        ParkourModeSuppressHook()
        {
            PresetScript_CCodeInTheMiddle(
                0x14273BC7B, 6,
                SuppressDisabledActionKeys,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                true);
            PresetScript_CCodeInTheMiddle(
                0x14273BED2, 5,
                SuppressDisabledActionKeysAfterPadFill,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                false);
            PresetScript_CCodeInTheMiddle(
                0x14273A721, 5,
                SuppressDisabledActionKeysAfterHardmappedPad,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES,
                false);
        }
    };

    AutoAssembleWrapper<ParkourModeSuppressHook>& GetParkourModeSuppressHook()
    {
        static AutoAssembleWrapper<ParkourModeSuppressHook> s_hook;
        return s_hook;
    }

    bool IsParkourSuppressSitePatched()
    {
        // Site 1 vanilla bytes: mov rdx,rsi ; mov rcx,rbx.
        const uint8_t* site1 = reinterpret_cast<const uint8_t*>(0x14273BC7B);
        if (site1[0] != 0x48 || site1[1] != 0x8B || site1[2] != 0xD6
            || site1[3] != 0x48 || site1[4] != 0x8B || site1[5] != 0xCB)
        {
            return true;
        }
        // Site 2 vanilla bytes: call FUN_14273c4c0 (E8 E9 05 00 00).
        const uint8_t* site2 = reinterpret_cast<const uint8_t*>(0x14273BED2);
        if (site2[0] != 0xE8 || site2[1] != 0xE9 || site2[2] != 0x05
            || site2[3] != 0x00 || site2[4] != 0x00)
        {
            return true;
        }
        // Site 3 vanilla bytes: call FUN_142735e20 (E8 FA B6 FF FF).
        const uint8_t* site3 = reinterpret_cast<const uint8_t*>(0x14273A721);
        if (site3[0] != 0xE8 || site3[1] != 0xFA || site3[2] != 0xB6
            || site3[3] != 0xFF || site3[4] != 0xFF)
        {
            return true;
        }
        return false;
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
             << "," << (override.expandRanges ? 1 : 0) << "\n";
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
    if (m_ParkourHookActive)
        ApplyParkourMode(false);
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
        if (m_ParkourHookActive)
            return;
        if (IsParkourSuppressSitePatched())
        {
            // Another plugin owns the keymap-tail site; do not fight over it.
            m_ParkourHookRefused = true;
            return;
        }
        m_ParkourHookRefused = false;
        g_ParkourModeInstance = this;
        GetParkourModeSuppressHook().Activate();
        m_ParkourHookActive = true;
    }
    else
    {
        if (!m_ParkourHookActive)
            return;
        GetParkourModeSuppressHook().Deactivate();
        if (g_ParkourModeInstance == this)
            g_ParkourModeInstance = nullptr;
        m_ParkourHookActive = false;
    }
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

    if (m_WaitingForKey)
    {
        if (captureKey(ActiveToggleKey()))
        {
            m_JustCapturedKey = ActiveToggleKey();
            m_WaitingForKey = false;
            SaveSettings();
        }
    }
    else if (m_WaitingForSnapKey)
    {
        if (captureKey(ActiveSnapDirectionKey()))
        {
            m_JustCapturedKey = ActiveSnapDirectionKey();
            m_WaitingForSnapKey = false;
            SaveSettings();
        }
    }
    else if (m_WaitingForParkourKey)
    {
        if (captureKey(ActiveParkourModeKey()))
        {
            m_JustCapturedKey = ActiveParkourModeKey();
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
            if (captureKey(override.keyCode))
            {
                m_JustCapturedKey = override.keyCode;
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
    if (wantParkourHook && !m_ParkourHookActive)
        ApplyParkourMode(true);
    else if (!wantParkourHook && m_ParkourHookActive)
        ApplyParkourMode(false);

    const bool needHook = freeJumpHeld || anyOverrideActive || m_SnapDirectionMode;

    if (needHook && !m_HookActive)
    {
        if (IsSortAndSelectSiteAlreadyPatched())
        {
            // Refuse a second full-function replacement. The UI reports this
            // state so the user can disable the conflicting parkour plugin.
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
            "Parkour sort/select (0x140133B00) is already patched by another plugin. "
            "FreeJump hook was not installed. Disable/remove the conflicting parkour "
            "plugin (for example ACU_ParkourPatches.dll) before testing these features.");
    }
    if (m_ParkourHookRefused)
    {
        ImGui::TextWrapped(
            "An input-update site used by Parkour Mode (0x14273BC7B keyboard/mouse, "
            "0x14273BED2 gamepad) is already patched by another plugin, so Parkour Mode "
            "was not installed. Remove the conflicting input-modifying plugin, then "
            "toggle Parkour Mode again.");
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
            if (ImGui::Button("Restore defaults (LMB/RMB/Crouch)"))
            {
                m_DisabledActionMask = (1u << 1) | (1u << 11) | (1u << 10);
                maskChanged = true;
            }
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
                if (ImGui::CollapsingHeader(("Custom " + std::to_string(idx)).c_str()))
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
