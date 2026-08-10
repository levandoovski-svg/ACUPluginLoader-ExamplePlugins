#include "pch.h"
#include "SidehopContextPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include "ACU/SmallArray.h"
#include <shlobj.h>
#include <fstream>

// Inline definition (not compiling AvailableParkourAction.cpp separately)
EnumParkourAction AvailableParkourAction::GetEnumParkourAction()
{
    return GET_AND_CAST_FANCY_FUNC(*this, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)(this);
}

static bool g_HooksEnabled = false;

static float g_HeightDiffMin = -3.0f;
static float g_HeightDiffMax = 1.5f;
static float g_ExpectedHeightDiff = -2.0f;
static float g_ExpectedVertSpeed = 9.0f;
static float g_ExpectedHorizSpeed = 4.0f;
static float g_CurveMax = 2.0f;
static float g_CurveMin = -4.0f;
static float g_ExpectedCurveMin = -2.0f;

static void BoostSidehopActions(AllRegisters* params)
{
    uintptr_t rsp = params->GetRSP();
    auto& actions = *(SmallArray<AvailableParkourAction*>*)(*(uint64*)(rsp + 0x48));

    // Sidehop boosting (only when sidehop mode is active)
    if (g_HooksEnabled)
    {
        for (int i = 0; i < actions.size; i++)
        {
            AvailableParkourAction* action = actions[i];
            if (!action) continue;
            if (action->GetEnumParkourAction() != EnumParkourAction::climbFacade_fromWallDescentSidewaysToGround_alsoSidehop)
                continue;
            action->fitness = 9999.0f;
            action->heightDifferenceMin = g_HeightDiffMin;
            action->heightDifferenceMax = g_HeightDiffMax;
            action->expectedHeightDiff_mb = g_ExpectedHeightDiff;
            action->expectedVerticalSpeed_mb = g_ExpectedVertSpeed;
            action->expectedHorizontalSpeed = g_ExpectedHorizSpeed;
            action->curveAllowedRangeMax = g_CurveMax;
            action->curveAllowedRangeMin = g_CurveMin;
            action->expectedCurveRangeMin = g_ExpectedCurveMin;
        }
    }
    // Always dispatch GenericHooksInParkourFiltering callbacks (for FreeJumpPlugin etc.)
    auto& gphCallbacks = GenericHooksInParkourFiltering::GetSingleton()->m_Callbacks;
    for (auto* cb : gphCallbacks)
    {
        if (cb->ChooseBeforeFiltering_fnp)
            cb->ChooseBeforeFiltering_fnp(cb->m_UserData, actions);
    }
}

SidehopEnablerHooks::SidehopEnablerHooks()
{
    PresetScript_CCodeInTheMiddle(0x14013493E, 7,
        BoostSidehopActions,
        AutoAssemblerCodeHolder_Base::RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
}

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\SidehopContextPlugin.ini";
    return "SidehopContextPlugin.ini";
}

void SidehopContextPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("Hotkey=", 0) == 0)
            try { m_Hotkey = std::stoi(line.substr(7), nullptr, 16); } catch (...) {}
        else if (line.rfind("Enabled=", 0) == 0)
            m_PluginEnabled = (line.substr(8) == "1");
        else if (line.rfind("HeightDiffMin=", 0) == 0)
            try { m_RangeHeightDiffMin = std::stof(line.substr(14)); } catch (...) {}
        else if (line.rfind("HeightDiffMax=", 0) == 0)
            try { m_RangeHeightDiffMax = std::stof(line.substr(14)); } catch (...) {}
        else if (line.rfind("ExpectedHeightDiff=", 0) == 0)
            try { m_RangeExpectedHeightDiff = std::stof(line.substr(19)); } catch (...) {}
        else if (line.rfind("ExpectedVertSpeed=", 0) == 0)
            try { m_RangeExpectedVertSpeed = std::stof(line.substr(18)); } catch (...) {}
        else if (line.rfind("ExpectedHorizSpeed=", 0) == 0)
            try { m_RangeExpectedHorizSpeed = std::stof(line.substr(19)); } catch (...) {}
        else if (line.rfind("CurveMax=", 0) == 0)
            try { m_RangeCurveMax = std::stof(line.substr(9)); } catch (...) {}
        else if (line.rfind("CurveMin=", 0) == 0)
            try { m_RangeCurveMin = std::stof(line.substr(9)); } catch (...) {}
        else if (line.rfind("ExpectedCurveMin=", 0) == 0)
            try { m_RangeExpectedCurveMin = std::stof(line.substr(17)); } catch (...) {}
    }
}

void SidehopContextPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetIniPath());
        if (file)
        {
            file << "Hotkey=" << std::hex << m_Hotkey << std::dec << "\n";
            file << "Enabled=" << (m_PluginEnabled ? 1 : 0) << "\n";
            file << "HeightDiffMin=" << m_RangeHeightDiffMin << "\n";
            file << "HeightDiffMax=" << m_RangeHeightDiffMax << "\n";
            file << "ExpectedHeightDiff=" << m_RangeExpectedHeightDiff << "\n";
            file << "ExpectedVertSpeed=" << m_RangeExpectedVertSpeed << "\n";
            file << "ExpectedHorizSpeed=" << m_RangeExpectedHorizSpeed << "\n";
            file << "CurveMax=" << m_RangeCurveMax << "\n";
            file << "CurveMin=" << m_RangeCurveMin << "\n";
            file << "ExpectedCurveMin=" << m_RangeExpectedCurveMin << "\n";
        }
    } catch (...) {}
}

SidehopContextPlugin::SidehopContextPlugin()
{
}

SidehopContextPlugin::~SidehopContextPlugin()
{
    if (m_HooksActive)
    {
        m_Hooks.Deactivate();
        m_HooksActive = false;
    }
}

void SidehopContextPlugin::OnUpdate()
{
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (GetAsyncKeyState(vk) & 1)
            {
                m_Hotkey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
        return;
    }

    bool holding = GetAsyncKeyState(m_Hotkey) & 0x8000;
    bool shouldBeActive = m_PluginEnabled && holding;

    if (shouldBeActive)
    {
        g_HeightDiffMin = m_RangeHeightDiffMin;
        g_HeightDiffMax = m_RangeHeightDiffMax;
        g_ExpectedHeightDiff = m_RangeExpectedHeightDiff;
        g_ExpectedVertSpeed = m_RangeExpectedVertSpeed;
        g_ExpectedHorizSpeed = m_RangeExpectedHorizSpeed;
        g_CurveMax = m_RangeCurveMax;
        g_CurveMin = m_RangeCurveMin;
        g_ExpectedCurveMin = m_RangeExpectedCurveMin;
    }

    g_HooksEnabled = shouldBeActive;

    if (shouldBeActive && !m_HooksActive)
    {
        m_Hooks.Activate();
        m_HooksActive = true;
    }
    else if (!shouldBeActive && m_HooksActive)
    {
        m_Hooks.Deactivate();
        m_HooksActive = false;
    }
}

static const char* const kKeyNames[] = {
    "None", "LMB", "RMB", "Cancel", "MB4", "MB5",
    "", "", "Backspace", "Tab", "", "", "Clear", "Enter",
    "", "", "Shift", "Ctrl", "Alt", "Pause", "Caps",
    "", "", "", "", "", "", "Escape",
    "", "", "", "", "Space", "PageUp", "PageDown", "End", "Home",
    "Left", "Up", "Right", "Down",
    "", "", "", "Print", "Insert", "Delete",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "", "", "", "", "", "", "",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "LWin", "RWin", "Apps",
    "", "", "Sleep",
    "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
    "Multiply", "Add", "Separator", "Subtract", "Decimal", "Divide",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "", "", "", "", "", "", "", "",
    "NumLock", "ScrollLock",
    "", "", "", "", "", "", "", "", "", "",
    "LShift", "RShift", "LCtrl", "RCtrl", "LAlt", "RAlt", "", "",
};

void SidehopContextPlugin::OnImGuiRender()
{
    ImGui::Text("Sidehop Context Plugin");
    ImGui::Separator();
    ImGui::TextWrapped("Hold keybind to boost sidehop action fitness so they survive "
                       "filtering — enabling sidehop/climbfacade in more walling contexts. "
                       "Release to restore normal behavior.");

    bool enabled = m_PluginEnabled;
    if (ImGui::Checkbox("Enable Sidehop Context", &enabled))
    {
        m_PluginEnabled = enabled;
        if (!enabled && m_HooksActive)
        {
            m_Hooks.Deactivate();
            m_HooksActive = false;
        }
        SaveSettings();
    }

    if (m_PluginEnabled)
    {
        ImGui::Indent();

        bool isActive = m_HooksActive;
        ImGui::Text("Status:");
        ImGui::SameLine();
        ImGui::TextColored(isActive ? ImVec4(0.2f, 0.9f, 0.3f, 1.0f) : ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
                           isActive ? "ACTIVE (holding key)" : "INACTIVE");

        int currentKey = m_Hotkey;
        if (currentKey < 0 || currentKey >= IM_ARRAYSIZE(kKeyNames)) currentKey = 0;
        const char* preview = kKeyNames[currentKey];
        if (!preview || !preview[0]) preview = "Unknown";

        if (ImGui::BeginCombo("Keybind", preview))
        {
            for (int i = 1; i < IM_ARRAYSIZE(kKeyNames); i++)
            {
                if (!kKeyNames[i] || !kKeyNames[i][0]) continue;
                bool isSelected = (m_Hotkey == i);
                if (ImGui::Selectable(kKeyNames[i], isSelected))
                {
                    m_Hotkey = i;
                    SaveSettings();
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (m_WaitingForKey)
        {
            ImGui::SameLine();
            if (ImGui::Button("Cancel Rebind"))
                m_WaitingForKey = false;
        }

        ImGui::Separator();
        ImGui::Text("Range Extension (applied to sidehop actions):");

        auto Slider = [&](const char* label, float* val, float min, float max, const char* fmt) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat(label, val, 0.1f, 1.0f, fmt))
            {
                if (*val < min) *val = min;
                if (*val > max) *val = max;
                SaveSettings();
            }
        };

        Slider("Height Min", &m_RangeHeightDiffMin, -20.0f, 20.0f, "%.1f");
        Slider("Height Max", &m_RangeHeightDiffMax, -20.0f, 20.0f, "%.1f");
        Slider("Expected Height", &m_RangeExpectedHeightDiff, -20.0f, 20.0f, "%.1f");
        Slider("Vert Speed", &m_RangeExpectedVertSpeed, 0.0f, 20.0f, "%.1f");
        Slider("Horiz Speed", &m_RangeExpectedHorizSpeed, 0.0f, 20.0f, "%.1f");
        Slider("Curve Max", &m_RangeCurveMax, -10.0f, 10.0f, "%.1f");
        Slider("Curve Min", &m_RangeCurveMin, -10.0f, 10.0f, "%.1f");
        Slider("Expected Curve Min", &m_RangeExpectedCurveMin, -10.0f, 10.0f, "%.1f");

        ImGui::Unindent();
    }
}
