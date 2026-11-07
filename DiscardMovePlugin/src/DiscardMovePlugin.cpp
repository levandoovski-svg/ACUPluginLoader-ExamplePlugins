#include "pch.h"
#include "DiscardMovePlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/GetTypeIntSafe.h"
#include "imgui/imgui.h"
#include <fstream>
#include <shlobj.h>
#include <cstdio>
#include <sstream>

// User-configurable discarder: any picked parkour move (EnumParkourAction) is
// force-discarded at creation through the vanilla destroy path - the same hook
// site and semantics FreeFormWallrun uses for wallruns 53/54. Only the
// force-discard callback is used (no keep-alive override, no sort&select
// ownership, no vtable swapping), and only while the hotkey is ACTIVE.

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\DiscardMovePlugin.ini";
    return "DiscardMovePlugin.ini";
}

// ---------------------------------------------------------------
// Gamepad (XInput) input, same mechanism as FancyVTableSwapPlugin /
// FreeJumpPlugin. Dynamic load: the game already imports xinput1_3.dll,
// so an XInput-capable pad is supported.
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
    static const char* names[DiscardPad_COUNT] = {
        "Pad DPad Up", "Pad DPad Down", "Pad DPad Left", "Pad DPad Right",
        "Pad Start", "Pad Back",
        "Pad L3", "Pad R3",
        "Pad LB", "Pad RB",
        "Pad A", "Pad B", "Pad X", "Pad Y",
        "Pad Left Trigger", "Pad Right Trigger",
        "Pad LStick Up", "Pad LStick Down", "Pad LStick Left", "Pad LStick Right",
        "Pad RStick Up", "Pad RStick Down", "Pad RStick Left", "Pad RStick Right",
    };
    return names[key - kDiscardPadKeyBase];
}

std::string KeyCodeLabel(int key)
{
    if (key >= kDiscardPadKeyBase)
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

DiscardMovePlugin::DiscardMovePlugin()
{
    m_Callbacks.ForceCreationDiscard_fnp = [](void* userData, AvailableParkourAction& action) -> bool {
        return static_cast<DiscardMovePlugin*>(userData)->OnForceCreationDiscard(action);
    };
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 4.0f;
    m_Callbacks.m_Name = "DiscardMovePlugin_DiscardChosenMoves";
    LoadSettings();
}

DiscardMovePlugin::~DiscardMovePlugin()
{
    if (m_ActivatorCreation)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_ActivatorCreation.reset();
    }
}

void DiscardMovePlugin::OnBeforeActivate()
{
    GenericHooksInParkourFiltering::GetSingleton()->Subscribe(m_Callbacks);
    RefreshHookOwnership();
}

void DiscardMovePlugin::RefreshHookOwnership()
{
    if (m_ActivatorCreation) return;
    m_ActivatorCreation = GenericHooksInParkourFiltering::GetSingleton()->RequestGPHCreation();
}

bool DiscardMovePlugin::IsPadCode(int key) const
{
    return key >= kDiscardPadKeyBase && key < kDiscardPadKeyBase + DiscardPad_COUNT;
}

// Hold mode: bound keyboard/mouse key or XInput pad input held right now.
// Toggle mode: m_ToggleState, flipped by the rising edge in OnUpdate.
bool DiscardMovePlugin::IsBoundKeyDown() const
{
    if (IsPadCode(m_Hotkey))
        return m_PadDown[m_Hotkey - kDiscardPadKeyBase];
    return (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
}

bool DiscardMovePlugin::IsActive() const
{
    if (!m_Enabled) return false;
    if (m_HoldMode) return IsBoundKeyDown();
    return m_ToggleState;
}

void DiscardMovePlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
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
            else if (line.rfind("Discard=", 0) == 0)
            {
                m_DiscardList.clear();
                std::istringstream csv(line.substr(8));
                std::string name;
                while (std::getline(csv, name, ','))
                {
                    if (name.empty()) continue;
                    // Names are the canonical form; a bare number is accepted too.
                    auto byName = enum_reflection<EnumParkourAction>::GetValue(name);
                    if (byName)
                        m_DiscardList.push_back(*byName);
                    else
                    {
                        try {
                            int numeric = std::stoi(name);
                            if (enum_reflection<EnumParkourAction>::Contains(numeric))
                                m_DiscardList.push_back((EnumParkourAction)numeric);
                        } catch (...) {}
                    }
                }
            }
        }
        catch (...) {}
    }
    const bool padKey = m_Hotkey >= kDiscardPadKeyBase && m_Hotkey < kDiscardPadKeyBase + DiscardPad_COUNT;
    if ((m_Hotkey < 1 || m_Hotkey > 0xFE) && !padKey) m_Hotkey = 0x77;
}

void DiscardMovePlugin::SaveSettings() const
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
            file << "Discard=";
            for (size_t i = 0; i < m_DiscardList.size(); ++i)
            {
                if (i) file << ",";
                file << enum_reflection<EnumParkourAction>::GetString(m_DiscardList[i]);
            }
            file << "\n";
        }
    } catch (...) {}
}

bool DiscardMovePlugin::IsDiscarded(int actionType) const
{
    for (EnumParkourAction move : m_DiscardList)
        if (actionType == (int)move)
            return true;
    return false;
}

// While ACTIVE, discard action types the user picked, via the vanilla destroy
// path (same shape as a native reject). Runs after all keep-alive overrides,
// so a keep-alive from another plugin cannot resurrect a move listed here.
// Also records everything seen at the hook into the diagnostics ring.
bool DiscardMovePlugin::OnForceCreationDiscard(AvailableParkourAction& action)
{
    const int type = GetTypeIntSafe(&action);
    const bool willDiscard = IsActive() && IsDiscarded(type);
    const int slot = ((m_DiagWriteIdx.fetch_add(1, std::memory_order_relaxed) % kDiagCount) + kDiagCount) % kDiagCount;
    m_Diag[slot] = { type, willDiscard, willDiscard };
    return willDiscard;
}

void DiscardMovePlugin::UpdatePadStates()
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
        auto mark = [this](DiscardPadCode code) { m_PadDown[code] = true; };
        if (state.buttons & 0x0001) mark(DiscardPad_DPadUp);
        if (state.buttons & 0x0002) mark(DiscardPad_DPadDown);
        if (state.buttons & 0x0004) mark(DiscardPad_DPadLeft);
        if (state.buttons & 0x0008) mark(DiscardPad_DPadRight);
        if (state.buttons & 0x0010) mark(DiscardPad_Start);
        if (state.buttons & 0x0020) mark(DiscardPad_Back);
        if (state.buttons & 0x0040) mark(DiscardPad_L3);
        if (state.buttons & 0x0080) mark(DiscardPad_R3);
        if (state.buttons & 0x0100) mark(DiscardPad_LB);
        if (state.buttons & 0x0200) mark(DiscardPad_RB);
        if (state.buttons & 0x1000) mark(DiscardPad_A);
        if (state.buttons & 0x2000) mark(DiscardPad_B);
        if (state.buttons & 0x4000) mark(DiscardPad_X);
        if (state.buttons & 0x8000) mark(DiscardPad_Y);
        if (state.leftTrigger > kTriggerThreshold)  mark(DiscardPad_LeftTrigger);
        if (state.rightTrigger > kTriggerThreshold) mark(DiscardPad_RightTrigger);
        if (state.thumbLY > kLeftThumbDeadzone)  mark(DiscardPad_LeftStickUp);
        if (state.thumbLY < -kLeftThumbDeadzone) mark(DiscardPad_LeftStickDown);
        if (state.thumbLX < -kLeftThumbDeadzone) mark(DiscardPad_LeftStickLeft);
        if (state.thumbLX > kLeftThumbDeadzone) mark(DiscardPad_LeftStickRight);
        if (state.thumbRY > kLeftThumbDeadzone)  mark(DiscardPad_RightStickUp);
        if (state.thumbRY < -kLeftThumbDeadzone) mark(DiscardPad_RightStickDown);
        if (state.thumbRX < -kLeftThumbDeadzone) mark(DiscardPad_RightStickLeft);
        if (state.thumbRX > kLeftThumbDeadzone) mark(DiscardPad_RightStickRight);
    }
}

// Input polling: fresh-press edges over the whole keyboard/mouse range plus
// the XInput pad codes (never the unreliable GetAsyncKeyState low bit).
// Runs on every frame, so rebinding works and toggles fire even while the
// plugin is disabled.
void DiscardMovePlugin::OnUpdate()
{
    UpdatePadStates();
    for (int vk = 1; vk <= 0xFE; ++vk)
    {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        m_KbPressed[vk] = down && !m_KbDownPrev[vk];
        m_KbDownPrev[vk] = down;
    }
    std::array<bool, DiscardPad_COUNT> padEdges {};
    for (int c = 0; c < DiscardPad_COUNT; ++c)
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
            ? padEdges[m_Hotkey - kDiscardPadKeyBase]
            : m_KbPressed[m_Hotkey];
        if (pressed)
        {
            m_ToggleState = !m_ToggleState;
            SaveSettings();
        }
    }
}

void DiscardMovePlugin::CaptureBinding(const std::array<bool, DiscardPad_COUNT>& padEdges)
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
    for (int c = 0; c < DiscardPad_COUNT; ++c)
    {
        if (padEdges[c])
        {
            m_Hotkey = kDiscardPadKeyBase + c;
            m_WaitingForKey = false;
            SaveSettings();
            return;
        }
    }
}

void DiscardMovePlugin::OnImGuiRender()
{
    bool enabled = m_Enabled;
    if (ImGui::Checkbox("Enable DiscardMove (discard chosen moves while ACTIVE)", &enabled))
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

    ImGui::Separator();
    ImGui::TextWrapped("Moves discarded at creation while the hotkey is ACTIVE:");

    // Picker over every EnumParkourAction value.
    constexpr size_t numMoves = enum_reflection<EnumParkourAction>::GetNumItems();
    static constexpr auto allNames = enum_reflection<EnumParkourAction>::GetAllStrings();
    static constexpr auto allValues = enum_reflection<EnumParkourAction>::GetAllValues();
    if (ImGui::BeginCombo("##newMovePick", allNames[m_PendingPickIdx]))
    {
        for (size_t i = 0; i < numMoves; ++i)
        {
            const bool isSelected = (m_PendingPickIdx == (int)i);
            if (ImGui::Selectable(allNames[i], isSelected))
                m_PendingPickIdx = (int)i;
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const EnumParkourAction pendingMove = allValues[m_PendingPickIdx];
    const bool alreadyListed = std::any_of(m_DiscardList.begin(), m_DiscardList.end(),
        [&](EnumParkourAction move) { return move == pendingMove; });
    ImGui::BeginDisabled(alreadyListed);
    if (ImGui::Button("Add move to discard list"))
    {
        m_DiscardList.push_back(pendingMove);
        SaveSettings();
    }
    ImGui::EndDisabled();

    for (size_t i = 0; i < m_DiscardList.size();)
    {
        ImGui::PushID((int)i);
        ImGui::BulletText("%s", enum_reflection<EnumParkourAction>::GetString(m_DiscardList[i]));
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
        {
            m_DiscardList.erase(m_DiscardList.begin() + i);
            SaveSettings();
        }
        else
            ++i;
        ImGui::PopID();
    }
    if (m_DiscardList.empty())
        ImGui::TextDisabled("(empty - no moves are discarded)");

    ImGui::Separator();
    ImGui::Checkbox("Diagnostics: last actions seen at the discard hook", &m_ShowDiagnostics);
    if (!m_ShowDiagnostics) return;
    ImGui::TextDisabled("Type 'unknown' means the type could not be read for that action.");
    if (ImGui::BeginTable("discardDiag", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Discarded?");
        ImGui::TableHeadersRow();
        // Newest first.
        int writeIdx = m_DiagWriteIdx.load(std::memory_order_relaxed);
        for (int j = 0; j < kDiagCount; ++j)
        {
            const int idx = writeIdx - 1 - j;
            if (idx < 0) break;
            const int slot = ((idx % kDiagCount) + kDiagCount) % kDiagCount;
            const DiagEntry& e = m_Diag[slot];
            if (e.type <= -2) continue; // slot never written
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", e.type);
            ImGui::TableNextColumn();
            ImGui::Text(e.type < 0 ? "(unreadable)" 
                : enum_reflection<EnumParkourAction>::GetString((EnumParkourAction)e.type));
            ImGui::TableNextColumn();
            ImGui::Text(e.discardApplied ? "YES" : "no");
        }
        ImGui::EndTable();
    }
}
