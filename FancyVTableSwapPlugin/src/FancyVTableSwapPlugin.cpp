#include "pch.h"
#include "FancyVTableSwapPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/GetTypeIntSafe.h"
#include "imgui/imgui.h"
#include <fstream>
#include <shlobj.h>
#include <cstdio>
#include <sstream>

// Passive freeform-wallrun plugin (the proven mode):
// while the hotkey is ACTIVE, the game's own wallrun candidates are discarded
// at creation through the vanilla reject path, and natural freeform 52s are
// kept alive. The game then picks among the surviving candidates itself.
//
// Deliberately NOT done (each was tested and/or chased across sessions):
//   - no sort&select site ownership: FreeJump's FinalFilter1 fallback cave
//     (per-candidate decide calls + wild calls on poisoned candidates) only
//     engages when another plugin machine-patches that site. Leaving the site
//     alone is what fixed the single-digit-FPS and the held-too-long crash.
//   - no 53->52 vtable conversion: a converted 53 runs with 53-computed
//     trajectory data and behaves unpredictably.
//   - no weight boost / force-select / fitness-cull edits.
//   - no teleport lift.

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\FreeFormWallrun.ini";
    return "FreeFormWallrun.ini";
}
// Pre-rename INI, read once on load so existing hotkey settings survive.
static std::string GetLegacyIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\FancyVTableSwapPlugin.ini";
    return "FancyVTableSwapPlugin.ini";
}

// ---------------------------------------------------------------
// Gamepad (XInput) input, same mechanism as FreeJumpPlugin /
// ExactPositionTeleportPlugin. Dynamic load: the game already
// imports xinput1_3.dll, so an XInput-capable pad is supported.
namespace {
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

const char* PadCodeName(int key)
{
    static const char* names[WallrunPad_COUNT] = {
        "Pad DPad Up", "Pad DPad Down", "Pad DPad Left", "Pad DPad Right",
        "Pad Start", "Pad Back",
        "Pad L3", "Pad R3",
        "Pad LB", "Pad RB",
        "Pad A", "Pad B", "Pad X", "Pad Y",
        "Pad Left Trigger", "Pad Right Trigger",
        "Pad LStick Up", "Pad LStick Down", "Pad LStick Left", "Pad LStick Right",
        "Pad RStick Up", "Pad RStick Down", "Pad RStick Left", "Pad RStick Right",
    };
    return names[key - kWallrunPadKeyBase];
}

std::string KeyCodeLabel(int key)
{
    if (key >= kWallrunPadKeyBase)
    {
        std::ostringstream label;
        label << "0x" << std::uppercase << std::hex << key << " (" << PadCodeName(key) << ")";
        return label.str();
    }
    const char* mouseName = nullptr;
    switch (key)
    {
    case VK_LBUTTON:  mouseName = "Mouse Left"; break;
    case VK_RBUTTON:  mouseName = "Mouse Right"; break;
    case VK_MBUTTON:  mouseName = "Mouse Middle"; break;
    case VK_XBUTTON1: mouseName = "Mouse X1"; break;
    case VK_XBUTTON2: mouseName = "Mouse X2"; break;
    default: break;
    }
    std::ostringstream label;
    label << "0x" << std::uppercase << std::hex << key;
    if (mouseName) label << " (" << mouseName << ")";
    return label.str();
}
} // namespace

FancyVTableSwapPlugin::FancyVTableSwapPlugin()
{
    m_Callbacks.OverrideCreationDiscard_fnp = [](void* userData, AvailableParkourAction& action) -> bool {
        return static_cast<FancyVTableSwapPlugin*>(userData)->OnCreationDiscardOverride(action);
    };
    m_Callbacks.ForceCreationDiscard_fnp = [](void* userData, AvailableParkourAction& action) -> bool {
        return static_cast<FancyVTableSwapPlugin*>(userData)->OnForceCreationDiscard(action);
    };
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 4.0f;
    m_Callbacks.m_Name = "FreeFormWallrun_DiscardWallruns";
    LoadSettings();
}

FancyVTableSwapPlugin::~FancyVTableSwapPlugin()
{
    if (m_ActivatorCreation)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_ActivatorCreation.reset();
    }
}

void FancyVTableSwapPlugin::OnBeforeActivate()
{
    GenericHooksInParkourFiltering::GetSingleton()->Subscribe(m_Callbacks);
    RefreshHookOwnership();
}

// Only the creation hooks are ours, ever. The sort&select site stays unpatched
// so FreeJump never engages its fallback cave (see file comment).
void FancyVTableSwapPlugin::RefreshHookOwnership()
{
    if (m_ActivatorCreation) return;
    m_ActivatorCreation = GenericHooksInParkourFiltering::GetSingleton()->RequestGPHCreation();
}

bool FancyVTableSwapPlugin::IsPadCode(int key) const
{
    return key >= kWallrunPadKeyBase && key < kWallrunPadKeyBase + WallrunPad_COUNT;
}

// Hold mode: bound keyboard/mouse key or XInput pad input held right now.
// Toggle mode: m_ToggleState, flipped by the rising edge in OnUpdate.
bool FancyVTableSwapPlugin::IsBoundKeyDown() const
{
    if (IsPadCode(m_Hotkey))
        return m_PadDown[m_Hotkey - kWallrunPadKeyBase];
    return (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
}

bool FancyVTableSwapPlugin::IsActive() const
{
    if (!m_Enabled) return false;
    if (m_HoldMode) return IsBoundKeyDown();
    return m_ToggleState;
}

void FancyVTableSwapPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) { file.clear(); file.open(GetLegacyIniPath()); }
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        try
        {
            if (line.rfind("Hotkey=", 0) == 0)
                try { m_Hotkey = std::stoi(line.substr(7), nullptr, 16); } catch (...) {}
            else if (line.rfind("Enabled=", 0) == 0)
                m_Enabled = (line.substr(8) == "1");
            else if (line.rfind("HoldMode=", 0) == 0)
                try { m_HoldMode = std::stoi(line.substr(9)) != 0; } catch (...) {}
            else if (line.rfind("ToggleState=", 0) == 0)
                try { m_ToggleState = std::stoi(line.substr(12)) != 0; } catch (...) {}
        }
        catch (...) {}
    }
    const bool padKey = m_Hotkey >= kWallrunPadKeyBase && m_Hotkey < kWallrunPadKeyBase + WallrunPad_COUNT;
    if ((m_Hotkey < 1 || m_Hotkey > 0xFE) && !padKey) m_Hotkey = 0x77;
}

void FancyVTableSwapPlugin::SaveSettings() const
{
    try
    {
        std::ofstream file(GetIniPath());
        if (file)
        {
            file << "Hotkey=" << std::hex << m_Hotkey << std::dec << "\n";
            file << "Enabled=" << (m_Enabled ? 1 : 0) << "\n";
            file << "HoldMode=" << (m_HoldMode ? 1 : 0) << "\n";
            file << "ToggleState=" << (m_ToggleState ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

// Keep natural freeform 52s alive even when the vanilla verdict rejects them
// (on handhold walls the freeform gets discarded here - that is what we waive).
// NO vtable swapping: the move must be the one the game created itself, with
// its own coherent trajectory data.
bool FancyVTableSwapPlugin::OnCreationDiscardOverride(AvailableParkourAction& action)
{
    if (!IsActive()) return false;
    return GetTypeIntSafe(&action) == (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
}

// While armed, discard every naturally-created 53 (wallrunUpFromGround) and 54
// (wallrunUpToHang) upon creation via the vanilla destroy path, so they cannot
// win selection or split the freeform wallrun's decision. Natural 52s kept
// alive above survive and the game picks among them itself.
bool FancyVTableSwapPlugin::OnForceCreationDiscard(AvailableParkourAction& action)
{
    if (!IsActive()) return false;
    const int type = GetTypeIntSafe(&action);
    return type == (int)EnumParkourAction::wallrunUpFromGround_mb
        || type == (int)EnumParkourAction::wallrunUpToHang;
}

void FancyVTableSwapPlugin::UpdatePadStates()
{
    XInputGetState_t getState = GetXInputGetState();
    if (!getState) return;
    constexpr int kLeftThumbDeadzone = 7849;   // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
    constexpr int kTriggerThreshold = 30;      // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    constexpr int kPadSlotCount = 4;
    m_PadDown.fill(false);
    for (int slot = 0; slot < kPadSlotCount; ++slot)
    {
        XInputPadState state{};
        if (getState(slot, &state) != ERROR_SUCCESS)
            continue;
        auto mark = [this](WallrunPadCode code) { m_PadDown[code] = true; };
        if (state.buttons & 0x0001) mark(WallrunPad_DPadUp);
        if (state.buttons & 0x0002) mark(WallrunPad_DPadDown);
        if (state.buttons & 0x0004) mark(WallrunPad_DPadLeft);
        if (state.buttons & 0x0008) mark(WallrunPad_DPadRight);
        if (state.buttons & 0x0010) mark(WallrunPad_Start);
        if (state.buttons & 0x0020) mark(WallrunPad_Back);
        if (state.buttons & 0x0040) mark(WallrunPad_L3);
        if (state.buttons & 0x0080) mark(WallrunPad_R3);
        if (state.buttons & 0x0100) mark(WallrunPad_LB);
        if (state.buttons & 0x0200) mark(WallrunPad_RB);
        if (state.buttons & 0x1000) mark(WallrunPad_A);
        if (state.buttons & 0x2000) mark(WallrunPad_B);
        if (state.buttons & 0x4000) mark(WallrunPad_X);
        if (state.buttons & 0x8000) mark(WallrunPad_Y);
        if (state.leftTrigger > kTriggerThreshold)  mark(WallrunPad_LeftTrigger);
        if (state.rightTrigger > kTriggerThreshold) mark(WallrunPad_RightTrigger);
        if (state.thumbLY > kLeftThumbDeadzone)  mark(WallrunPad_LeftStickUp);
        if (state.thumbLY < -kLeftThumbDeadzone) mark(WallrunPad_LeftStickDown);
        if (state.thumbLX < -kLeftThumbDeadzone) mark(WallrunPad_LeftStickLeft);
        if (state.thumbLX > kLeftThumbDeadzone)  mark(WallrunPad_LeftStickRight);
        if (state.thumbRY > kLeftThumbDeadzone)  mark(WallrunPad_RightStickUp);
        if (state.thumbRY < -kLeftThumbDeadzone) mark(WallrunPad_RightStickDown);
        if (state.thumbRX < -kLeftThumbDeadzone) mark(WallrunPad_RightStickLeft);
        if (state.thumbRX > kLeftThumbDeadzone)  mark(WallrunPad_RightStickRight);
    }
}

// Input polling: fresh-press edges over the whole keyboard/mouse range plus
// the XInput pad codes (never the unreliable GetAsyncKeyState low bit).
// Runs on every frame, so rebinding works and toggles fire even while the
// plugin is disabled.
void FancyVTableSwapPlugin::OnUpdate()
{
    UpdatePadStates();
    for (int vk = 1; vk <= 0xFE; ++vk)
    {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        m_KbPressed[vk] = down && !m_KbDownPrev[vk];
        m_KbDownPrev[vk] = down;
    }
    std::array<bool, WallrunPad_COUNT> padEdges {};
    for (int c = 0; c < WallrunPad_COUNT; ++c)
    {
        padEdges[c] = m_PadDown[c] && !m_PadDownPrev[c];
        m_PadDownPrev[c] = m_PadDown[c];
    }

    if (m_WaitingForKey)
    {
        CaptureBinding(padEdges);
        // toggle-state tracking still works underneath
    }

    if (m_Enabled && !m_HoldMode)
    {
        const bool pressed = IsPadCode(m_Hotkey)
            ? padEdges[m_Hotkey - kWallrunPadKeyBase]
            : m_KbPressed[m_Hotkey];
        if (pressed)
        {
            m_ToggleState = !m_ToggleState;
            SaveSettings();
        }
    }
}

void FancyVTableSwapPlugin::CaptureBinding(const std::array<bool, WallrunPad_COUNT>& padEdges)
{
    for (int vk = 1; vk <= 0xFE; ++vk)
    {
        // The left/right mouse buttons belong to the loader UI and would
        // otherwise be captured by the click that starts a rebind. Middle
        // and extra mouse buttons bind like any other key.
        if (vk == VK_LBUTTON || vk == VK_RBUTTON)
            continue;
        if (m_KbPressed[vk])
        {
            m_Hotkey = vk;
            m_WaitingForKey = false;
            SaveSettings();
            return;
        }
    }
    for (int c = 0; c < WallrunPad_COUNT; ++c)
    {
        if (padEdges[c])
        {
            m_Hotkey = kWallrunPadKeyBase + c;
            m_WaitingForKey = false;
            SaveSettings();
            return;
        }
    }
}

void FancyVTableSwapPlugin::OnImGuiRender()
{
    bool enabled = m_Enabled;
    if (ImGui::Checkbox("Enable FreeFormWallrun (passive: natural freeform 52 only, hotkey ACTIVE)", &enabled))
    {
        m_Enabled = enabled;
        SaveSettings();
    }
    if (!m_Enabled) return;

    ImGui::Text("Status: %s", IsActive() ? "ACTIVE" : "INACTIVE");

    bool holdMode = m_HoldMode;
    if (ImGui::RadioButton("Hold", holdMode)) { m_HoldMode = true; SaveSettings(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Toggle", !holdMode)) { m_HoldMode = false; SaveSettings(); }

    ImGui::Text("Keybind: %s", KeyCodeLabel(m_Hotkey).c_str());
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey ? "Cancel rebind" : "Rebind"))
        m_WaitingForKey = !m_WaitingForKey;
    if (m_WaitingForKey)
        ImGui::TextWrapped("Press a keyboard key, mouse button or gamepad button to bind it...");
}
