#include "pch.h"
#include "FailedWallRunPlugin.h"
#include "ACU/basic_types.h"
#include "ACU/Entity.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/Memory/ACUAllocs.h"
#include "ACU_DefineNativeFunction.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include <fstream>
#include <shlobj.h>
#include "imgui/imgui.h"
#include <cstring>
#include <string_view>

#include "ACU/AtomGraph.h"
#include "ACU/AtomAnimComponent.h"
#include "ACU/HumanStatesHolder.h"

// Inline definition (not compiling AvailableParkourAction.cpp separately)
EnumParkourAction AvailableParkourAction::GetEnumParkourAction()
{
    return GET_AND_CAST_FANCY_FUNC(*this, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)(this);
}

// Extended action base with padding matching the game's full action size
class ParkourAction_Commonbase : public AvailableParkourAction
{
public:
    char pad_0290[0x2B0 - 0x290];
};
assert_sizeof(ParkourAction_Commonbase, 0x2B0);

DEFINE_GAME_FUNCTION(CreateParkourActionForParkourPointIfFits, 0x1401D1CF0, char, __fastcall,
    (EnumParkourAction a1, uint64 a2, __m128* a3, __m128* p_movementVecWorld_mb, float a5, int a6, char a7, uint64 a8, uint64 a9, uint64 p_currentLedge_mb, ParkourAction_Commonbase** p_newAction_out, float a12, float p_epsilon_mb));

namespace
{
    void SetGraphVariableInt(AtomGraph& atomGraph, uint32 rtcpIdx, int32 value)
    {
        if (!atomGraph.rtcp) return;
        uint32 offset = atomGraph.rtcp->graphVarsOffsets[rtcpIdx];
        *(int32*)(atomGraph.rtcp->graphVarsBuffer.arr + offset) = value;
    }
}

// Guard flag + cached settings for the static creation hook
static bool g_ShouldCreateFailedWallrun = false;
static float g_LiftAmount = 2.0f;
static float g_LiftSpeed = 6.0f;
static bool g_VtableSwapEnabled = false;
static uint64 g_Type52FancyVTable = 0;
static int g_Action52Count = 0;
static int g_SabotagedCount = 0;

// Permissive range values so injected type 52 survives validation
static float g_RangeHeightDiffMin = -3.0f;
static float g_RangeHeightDiffMax = 3.0f;
static float g_RangeHorizDiffMin = -3.0f;
static float g_RangeHorizDiffMax = 3.0f;
static float g_RangeDistMin = 0.0f;
static float g_RangeDistMax = 10.0f;
static float g_RangeCurveMax = 2.0f;
static float g_RangeCurveMin = -4.0f;
static float g_RangeExpectedCurveMin = -2.0f;

static bool IsTypeCompetingOnWalls(int t)
{
    return t == 53 || t == 65 || t == 73 || t == 32 || t == 44 || t == 77 || t == 70 || t == 35;
}
static bool IsTypeBeamSpecific(int t)
{
    return t == 10 || t == 12 || t == 60 || t == 100 || t == 101;
}

static void DuringClimbFacadeScan_CreateFailedWallrun(AllRegisters* params)
{
    if (!g_ShouldCreateFailedWallrun) return;

    // --- Count existing actions by type ---
    auto& existingActions = *(SmallArray<ParkourAction_Commonbase*>*)params->rbx_;
    int action52Count = 0;
    int beamTypeCount = 0;
    for (int i = 0; i < existingActions.size; ++i)
    {
        if (!existingActions[i]) continue;
        int t = (int)existingActions[i]->GetEnumParkourAction();
        if (t == 52) action52Count++;
        else if (IsTypeBeamSpecific(t)) beamTypeCount++;
    }

    // --- Create failed wallrun action ---
    ParkourAction_Commonbase* newAction = nullptr;
    bool isCreated = CreateParkourActionForParkourPointIfFits(
        EnumParkourAction::wallrunUpFromGroundFailed_mb,
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
    if (!isCreated || !newAction) return;

    action52Count++;

    // Expand allowed ranges so the injected action survives validation
    newAction->heightDifferenceMin = g_RangeHeightDiffMin;
    newAction->heightDifferenceMax = g_RangeHeightDiffMax;
    newAction->horizontalDifferenceMin = g_RangeHorizDiffMin;
    newAction->horizontalDifferenceMax = g_RangeHorizDiffMax;
    newAction->distanceMin = g_RangeDistMin;
    newAction->distanceMax = g_RangeDistMax;
    newAction->curveAllowedRangeMax = g_RangeCurveMax;
    newAction->curveAllowedRangeMin = g_RangeCurveMin;
    newAction->expectedCurveRangeMin = g_RangeExpectedCurveMin;

    // Boost fitness so it wins the sort
    newAction->fitness = 9999.0f;

    // Capture fancyVTable for optional vtable swap on beams
    if (g_Type52FancyVTable == 0)
        g_Type52FancyVTable = (uint64)newAction->fancyVTable;

    // Adjust destination height and expected speed for the lift animation
    {
        Vector3f& dest = (Vector3f&)newAction->locationAnchorDest;
        Vector3f& src = (Vector3f&)newAction->locationAnchorSrc;
        float topLedgeZ = newAction->locationTopLedge.z;
        float riseTarget = src.z + g_LiftAmount;
        if (topLedgeZ < 1e9f && topLedgeZ > src.z + 0.5f)
            riseTarget = std::min(topLedgeZ - 0.3f, src.z + g_LiftAmount + 1.0f);
        if (dest.z < riseTarget)
        {
            dest.z = riseTarget;
            newAction->expectedVerticalSpeed_mb = g_LiftSpeed;
            newAction->expectedHeightDiff_mb = riseTarget - src.z;
            newAction->expectedVerticalDefaultDisplace = -1.0f;
        }
    }

    // --- Sabotage competing actions in the existing array ---
    int sabotagedCount = 0;
    for (int i = 0; i < existingActions.size; ++i)
    {
        AvailableParkourAction* action = existingActions[i];
        if (!action) continue;
        int t = (int)action->GetEnumParkourAction();

        // On walls: sabotage wall competitors so type 52 wins
        if (action52Count > 0 && IsTypeCompetingOnWalls(t))
        {
            action->fitness = 0.0f;
            sabotagedCount++;
        }
    }

    // Append the new action
    ACU::Memory::SmallArrayAppend(existingActions, newAction);

    g_Action52Count = action52Count;
    g_SabotagedCount = sabotagedCount;
}

GPH_FailedWallrunCreator::GPH_FailedWallrunCreator()
{
    PresetScript_CCodeInTheMiddle(0x140135230, 12,
        DuringClimbFacadeScan_CreateFailedWallrun,
        AutoAssemblerCodeHolder_Base::RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
}

uint64 FailedWallRunPlugin::s_Type52FancyVTable = 0;

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\FailedWallRunPlugin.ini";
    return "FailedWallRunPlugin.ini";
}

void FailedWallRunPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("ToggleKey=", 0) == 0)
            try { m_ToggleKey = std::stoi(line.substr(10), nullptr, 16); } catch (...) {}
        else if (line.rfind("Enabled=", 0) == 0)
            m_PluginEnabled = (line.substr(8) == "1");
        else if (line.rfind("HoldMode=", 0) == 0)
            try { m_HoldMode = std::stoi(line.substr(9)) != 0; } catch (...) {}
        else if (line.rfind("ToggleState=", 0) == 0)
            try { m_ToggleState = std::stoi(line.substr(12)) != 0; } catch (...) {}
        else if (line.rfind("LiftAmount=", 0) == 0)
            try { m_LiftAmount = std::stof(line.substr(11)); } catch (...) {}
        else if (line.rfind("AnimationCancel=", 0) == 0)
            m_CancelEnabled = (line.substr(15) == "1");
        else if (line.rfind("LiftSpeed=", 0) == 0)
            try { m_LiftSpeed = std::stof(line.substr(10)); } catch (...) {}
        else if (line.rfind("EnforceEnabled=", 0) == 0)
            m_EnforceEnabled = (line.substr(15) == "1");
        else if (line.rfind("VtableSwapEnabled=", 0) == 0)
            m_VtableSwapEnabled = (line.substr(18) == "1");
    }
}

void FailedWallRunPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetIniPath());
        if (file)
        {
            file << "ToggleKey=" << std::hex << m_ToggleKey << std::dec << "\n";
            file << "Enabled=" << (m_PluginEnabled ? 1 : 0) << "\n";
            file << "HoldMode=" << (m_HoldMode ? 1 : 0) << "\n";
            file << "ToggleState=" << (m_ToggleState ? 1 : 0) << "\n";
            file << "LiftAmount=" << m_LiftAmount << "\n";
            file << "AnimationCancel=" << (m_CancelEnabled ? 1 : 0) << "\n";
            file << "LiftSpeed=" << m_LiftSpeed << "\n";
            file << "EnforceEnabled=" << (m_EnforceEnabled ? 1 : 0) << "\n";
            file << "VtableSwapEnabled=" << (m_VtableSwapEnabled ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

bool FailedWallRunPlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}

bool FailedWallRunPlugin::IsFeatureActive() const
{
    if (!m_PluginEnabled) return false;
    if (m_HoldMode)
        return (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    else
        return m_ToggleState;
}

FailedWallRunPlugin::FailedWallRunPlugin()
{
}

FailedWallRunPlugin::~FailedWallRunPlugin()
{
    if (m_CreatorHookActive)
    {
        m_FailedWallrunCreator.Deactivate();
        m_CreatorHookActive = false;
    }
}

void FailedWallRunPlugin::OnUpdate()
{
    // Handle key rebinding
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (IsRisingEdgePressed(vk))
            {
                m_ToggleKey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
        return;
    }

    // Update toggle state if in toggle mode
    if (m_PluginEnabled && !m_HoldMode)
    {
        if (IsRisingEdgePressed(m_ToggleKey))
        {
            m_ToggleState = !m_ToggleState;
            SaveSettings();
        }
    }

    // Sync globals for the static creation hook
    bool shouldBeActive = IsFeatureActive() && m_EnforceEnabled;
    g_ShouldCreateFailedWallrun = shouldBeActive;
    g_LiftAmount = m_LiftAmount;
    g_LiftSpeed = m_LiftSpeed;
    g_VtableSwapEnabled = m_VtableSwapEnabled;

    // Activate/deactivate the creation hook
    if (shouldBeActive && !m_CreatorHookActive)
    {
        m_FailedWallrunCreator.Activate();
        m_CreatorHookActive = true;
    }
    else if (!shouldBeActive && m_CreatorHookActive)
    {
        m_FailedWallrunCreator.Deactivate();
        m_CreatorHookActive = false;
    }

    // Refresh debug stats from the globals
    m_LastAction52Count = g_Action52Count;
    m_LastSabotagedCount = g_SabotagedCount;

    // --- Ezio-style Animation Cancel ---
    if (m_CancelEnabled)
    {
        if (IsRisingEdgePressed(VK_SPACE) && IsInParkourState())
        {
            HumanStatesHolder* holder = HumanStatesHolder::GetForPlayer();
            if (holder && holder->atomAnimCpnt && holder->atomAnimCpnt->shared_AtomGraph_NewDemo_DEV)
            {
                AtomGraph* graph = holder->atomAnimCpnt->shared_AtomGraph_NewDemo_DEV->GetPtr();
                if (graph && graph->rtcp)
                {
                    SetGraphVariableInt(*graph, kGeneralStateRTCPIdx, 0);
                    SetGraphVariableInt(*graph, kParkourRTCPIdx, 0);
                }
            }
        }
    }
}

bool FailedWallRunPlugin::IsInParkourState() const
{
    HumanStatesHolder* holder = HumanStatesHolder::GetForPlayer();
    if (!holder || !holder->atomAnimCpnt || !holder->atomAnimCpnt->shared_AtomGraph_NewDemo_DEV) return false;

    AtomGraph* graph = holder->atomAnimCpnt->shared_AtomGraph_NewDemo_DEV->GetPtr();
    if (!graph || !graph->rtcp) return false;

    uint32 offset = graph->rtcp->graphVarsOffsets[kGeneralStateRTCPIdx];
    int32 generalState = *(int32*)(graph->rtcp->graphVarsBuffer.arr + offset);
    return generalState == 5;
}

void FailedWallRunPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
    const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);
    const ImVec4 debugColor(0.60f, 0.60f, 1.00f, 1.00f);

    ImGui::Text("Failed Wall-Run Plugin");
    ImGui::Separator();

    ImGui::TextWrapped("Two features in one plugin: (1) Force failed wallrun to enable side ejects, "
                       "(2) Ezio-style animation cancel to interrupt any parkour action mid-animation.");
    ImGui::Spacing();

    // --- Section 1: Failed Wallrun ---
    if (ImGui::CollapsingHeader("Failed Wallrun Side-Eject", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Forces the failed wallrun animation, enabling side ejects "
                           "during the animation. Works from ground, beams, and ropes.");

        bool enabled = m_PluginEnabled;
        if (ImGui::Checkbox("Enable Failed Wallrun", &enabled))
        {
            m_PluginEnabled = enabled;
            SaveSettings();
        }

        if (m_PluginEnabled)
        {
            ImGui::Indent();

            bool isActive = IsFeatureActive();
            ImGui::Text("Status:"); ImGui::SameLine();
            ImGui::TextColored(isActive ? activeColor : inactiveColor,
                               isActive ? "ACTIVE" : "INACTIVE");

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
            if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind##fwr"))
                m_WaitingForKey = !m_WaitingForKey;

            ImGui::Separator();
            ImGui::TextColored(debugColor, "Tuning (textboxes accept manual input + Enter):");

            bool enforce = m_EnforceEnabled;
            if (ImGui::Checkbox("Enforce failed wallrun", &enforce))
            {
                m_EnforceEnabled = enforce;
                SaveSettings();
            }

            if (m_EnforceEnabled)
            {
                ImGui::Indent();

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

                ImGui::Text("Lift speed:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                float liftS = m_LiftSpeed;
                if (ImGui::InputFloat("m/s##liftspeed", &liftS, 0.5f, 1.0f, "%.1f"))
                {
                    if (liftS < 0.5f) liftS = 0.5f;
                    if (liftS > 20.0f) liftS = 20.0f;
                    m_LiftSpeed = liftS;
                    SaveSettings();
                }

                bool vtableSwap = m_VtableSwapEnabled;
                if (ImGui::Checkbox("Vtable swap (experimental, crash risk)", &vtableSwap))
                {
                    m_VtableSwapEnabled = vtableSwap;
                    SaveSettings();
                }

                ImGui::Unindent();
            }

            if (isActive)
            {
                ImGui::Separator();
                ImGui::TextColored(debugColor, "Stats:");
                ImGui::Text("  Action 52 found: %d | Sabotaged: %d",
                    m_LastAction52Count,
                    m_LastSabotagedCount);
                ImGui::Text("  Lift height: %.1fm | Speed: %.1f",
                    m_LiftAmount,
                    m_LiftSpeed);

                if (g_Type52FancyVTable == 0)
                    ImGui::TextColored(ImVec4(1,1,0,1), "  (capturing type 52 vtable... approach a wall)");
            }

            ImGui::Unindent();
        }
    }

    // --- Section 2: Animation Cancel ---
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Animation Cancel (Ezio-Style)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped(
            "Instantly cancels any parkour animation mid-motion when you press Space, "
            "letting you chain into a new action immediately. "
            "This replicates the responsive parkour feel from Assassin's Creed 2 / Brotherhood / Revelations.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "How to use: While Arno is in a parkour animation (wallrun, vault, side-eject, etc.), "
            "press Space to cancel. The animation will instantly blend to idle, "
            "and you can immediately press a new direction for the next parkour action.");
        ImGui::Spacing();

        bool cancelEnabled = m_CancelEnabled;
        if (ImGui::Checkbox("Enable Animation Cancel", &cancelEnabled))
        {
            m_CancelEnabled = cancelEnabled;
            SaveSettings();
        }

        if (m_CancelEnabled)
        {
            ImGui::Indent();
            ImGui::Separator();
            ImGui::TextColored(debugColor, "Status:");
            ImGui::Text("  In Parkour State: %s", IsInParkourState() ? "YES" : "NO");
            ImGui::Text("  Cancel Key: Space");
            ImGui::Unindent();
        }
    }
}
