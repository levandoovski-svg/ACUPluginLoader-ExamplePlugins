#include "pch.h"
#include "SwingFromHangPlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/Entity.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/Memory/ACUAllocs.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include <shlobj.h>
#include <fstream>

static bool g_SwingFromHangEnabled = false;
static int g_SwingType = 43;
static float g_LiftAmount = 1.5f;
static volatile bool g_GameReady = false;

class ParkourAction_Commonbase : public AvailableParkourAction
{
public:
    char pad_0290[0x2B0 - 0x290];
};
assert_sizeof(ParkourAction_Commonbase, 0x2B0);

DEFINE_GAME_FUNCTION(CreateParkourActionForParkourPointIfFits, 0x1401D1260, char, __fastcall,
    (EnumParkourAction a1, uint64 a2, __m128* a3, __m128* p_movementVecWorld_mb, float a5, int a6, char a7, uint64 a8, uint64 a9, uint64 p_currentLedge_mb, ParkourAction_Commonbase** p_newAction_out, float a12, float p_epsilon_mb));

static bool IsInHangStateSafe()
{
    if (!g_GameReady) return false;
    __try
    {
        HumanStatesHolder* holder = HumanStatesHolder::GetForPlayer();
        if (!holder) return false;
        for (auto& r : holder->primaryCallbackReceivers)
        {
            if (!r.pNode) continue;
            uint64_t addr = (uint64_t)r.pNode->Enter;
            if (addr == 0x141A93430)
                return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

bool SwingFromHangPlugin::IsInHangState() const
{
    return IsInHangStateSafe();
}

static void DuringClimbFacadeScan_CreateSwing(AllRegisters* params)
{
    if (!g_SwingFromHangEnabled) return;
    if (!IsInHangStateSafe()) return;

    __try
    {
        ParkourAction_Commonbase* newAction = nullptr;
        bool isCreated = CreateParkourActionForParkourPointIfFits(
            (EnumParkourAction)g_SwingType,
            params->rdx_,
            (__m128*)params->rdi_,
            (__m128*)params->r9_,
            params->XMM6.f0,
            (int&)params->r13_,
            *(char*)(params->GetRSP() + 0x100),
            *(uint64*)(*(uint64*)params->r12_ + (int&)params->r15_),
            *(uint64*)(params->rbp_ + 0x118),
            *(uint64*)(params->rbp_ + 0x120),
            &newAction,
            *(float*)(params->rbp_ + 0x104),
            *(float*)(params->rbp_ + 0x108)
        );
        if (isCreated && newAction)
        {
            newAction->fitness = 9999.0f;
            Vector3f& dest = (Vector3f&)newAction->locationAnchorDest;
            Vector3f& src = (Vector3f&)newAction->locationAnchorSrc;
            float riseTarget = src.z + g_LiftAmount;
            if (dest.z < riseTarget)
            {
                dest.z = riseTarget;
                newAction->expectedVerticalSpeed_mb = 3.0f;
                newAction->expectedHeightDiff_mb = riseTarget - src.z;
                newAction->expectedVerticalDefaultDisplace = -1.0f;
            }
            ACU::Memory::SmallArrayAppend(
                *(SmallArray<ParkourAction_Commonbase*>*)params->rbx_,
                newAction);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

struct SwingFromHangHook : AutoAssemblerCodeHolder_Base
{
    SwingFromHangHook()
    {
        PresetScript_CCodeInTheMiddle(0x140136070, 12,
            DuringClimbFacadeScan_CreateSwing,
            AutoAssemblerCodeHolder_Base::RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    }
};

static AutoAssembleWrapper<SwingFromHangHook> g_wrapper;
static volatile bool g_HookInstalled = false;

static void InstallHook()
{
    if (!g_HookInstalled)
    {
        // Hook disabled - crashes on 1.5.0
        // g_wrapper.Activate();
        g_HookInstalled = true;
    }
}

std::string SwingFromHangPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\SwingFromHangPlugin.ini";
    return "SwingFromHangPlugin.ini";
}

void SwingFromHangPlugin::LoadSettingsFile()
{
    std::ifstream file(GetIniPath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("ToggleKey=", 0) == 0)
            try { m_ToggleKey = std::stoi(line.substr(10), nullptr, 16); } catch (...) {}
        else if (line.rfind("Enabled=", 0) == 0)
            m_Enabled = (line.substr(8) == "1");
        else if (line.rfind("HoldMode=", 0) == 0)
            try { m_HoldMode = std::stoi(line.substr(9)) != 0; } catch (...) {}
        else if (line.rfind("ToggleState=", 0) == 0)
            try { m_ToggleState = std::stoi(line.substr(12)) != 0; } catch (...) {}
        else if (line.rfind("LiftAmount=", 0) == 0)
            try { m_LiftAmount = std::stof(line.substr(11)); } catch (...) {}
        else if (line.rfind("SwingType=", 0) == 0)
            try { m_SwingType = std::stoi(line.substr(10)); } catch (...) {}
        else if (line.rfind("DebugMode=", 0) == 0)
            m_DebugMode = (line.substr(10) == "1");
    }
}

void SwingFromHangPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetIniPath());
        if (file)
        {
            file << "ToggleKey=" << std::hex << m_ToggleKey << std::dec << "\n";
            file << "Enabled=" << (m_Enabled ? 1 : 0) << "\n";
            file << "HoldMode=" << (m_HoldMode ? 1 : 0) << "\n";
            file << "ToggleState=" << (m_ToggleState ? 1 : 0) << "\n";
            file << "LiftAmount=" << m_LiftAmount << "\n";
            file << "SwingType=" << m_SwingType << "\n";
            file << "DebugMode=" << (m_DebugMode ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

void SwingFromHangPlugin::LoadSettings()
{
    LoadSettingsFile();
    InstallHook();
}

bool SwingFromHangPlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}

bool SwingFromHangPlugin::IsFeatureActive() const
{
    if (!m_Enabled) return false;
    if (m_DebugMode) return (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    if (!IsInHangState()) return false;
    if (m_HoldMode)
        return (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    else
        return m_ToggleState;
}

void SwingFromHangPlugin::OnUpdate()
{
    if (!g_GameReady)
    {
        __try
        {
            auto* player = ACU::GetPlayer();
            auto* world = ACU::GetWorld();
            if (player && world)
                g_GameReady = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }

    if (m_WaitingForKey)
    {
        for (int vk = 8; vk <= 254; ++vk)
        {
            if ((GetAsyncKeyState(vk) & 0x8000) && !(vk >= VK_LSHIFT && vk <= VK_RMENU))
            {
                m_ToggleKey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
        return;
    }

    if (m_Enabled && !m_HoldMode)
    {
        if (IsRisingEdgePressed(m_ToggleKey))
        {
            m_ToggleState = !m_ToggleState;
            SaveSettings();
        }
    }

    g_SwingFromHangEnabled = IsFeatureActive();
    g_SwingType = m_SwingType;
    g_LiftAmount = m_LiftAmount;
}

void SwingFromHangPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
    const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);
    const ImVec4 debugColor(0.60f, 0.60f, 1.00f, 1.00f);

    if (!ImGui::CollapsingHeader("Swing From Hang"))
        return;

    ImGui::TextWrapped("Allows Arno to swing from any hang state.");
    ImGui::Spacing();

    bool enabled = m_Enabled;
    if (ImGui::Checkbox("Enable Swing From Hang", &enabled))
    {
        m_Enabled = enabled;
        SaveSettings();
    }

    if (m_Enabled)
    {
        ImGui::Indent();

        bool isActive = IsFeatureActive();
        ImGui::Text("Status:"); ImGui::SameLine();
        ImGui::TextColored(isActive ? activeColor : inactiveColor,
                           isActive ? "ACTIVE" : "INACTIVE");

        ImGui::Text("In Hang State: %s", IsInHangState() ? "YES" : "NO");

        bool holdMode = m_HoldMode;
        if (ImGui::RadioButton("Hold (key held)", holdMode))
        {
            m_HoldMode = true;
            SaveSettings();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Toggle (press to toggle)", !holdMode))
        {
            m_HoldMode = false;
            SaveSettings();
        }

        ImGui::Text("Trigger key: 0x%02X", m_ToggleKey);
        if (m_WaitingForKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Press any key..."); }
        if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind##sfh"))
            m_WaitingForKey = !m_WaitingForKey;

        ImGui::Separator();
        ImGui::TextColored(debugColor, "Tuning:");

        ImGui::Text("Swing type:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        int swingType = m_SwingType;
        const char* swingTypes[] = { "43 - swing_2B", "45 - swingTurn", "46 - swingToWalling", "47 - swingToSidewall", "48 - swingToFeet", "49 - swingToSwing" };
        int swingIndices[] = { 43, 45, 46, 47, 48, 49 };
        int currentIdx = 0;
        for (int i = 0; i < 6; i++)
        {
            if (swingIndices[i] == swingType) { currentIdx = i; break; }
        }
        if (ImGui::Combo("##swingtype", &currentIdx, swingTypes, 6))
        {
            m_SwingType = swingIndices[currentIdx];
            SaveSettings();
        }

        ImGui::Text("Lift height:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        float liftH = m_LiftAmount;
        if (ImGui::InputFloat("m##liftheight", &liftH, 0.1f, 0.5f, "%.1f"))
        {
            if (liftH < 0.0f) liftH = 0.0f;
            if (liftH > 10.0f) liftH = 10.0f;
            m_LiftAmount = liftH;
            SaveSettings();
        }

        bool debug = m_DebugMode;
        if (ImGui::Checkbox("Debug mode", &debug))
        {
            m_DebugMode = debug;
            SaveSettings();
        }

        if (isActive && m_DebugMode)
        {
            ImGui::Separator();
            ImGui::TextColored(debugColor, "Debug:");
            ImGui::Text("  Swing type: %d", m_SwingType);
            ImGui::Text("  Lift height: %.1fm", m_LiftAmount);
        }

        ImGui::Unindent();
    }
}
