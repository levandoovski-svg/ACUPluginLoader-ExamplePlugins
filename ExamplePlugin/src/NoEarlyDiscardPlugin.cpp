#include "pch.h"
#include "NoEarlyDiscardPlugin.h"
#include "ACU/Memory/ACUAllocs.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ACU_DefineNativeFunction.h"

class Entity;
struct CollisionProbeForParkour_mb;

DEFINE_GAME_FUNCTION(ConstructParkourAction_A, 0x1401CBF00, AvailableParkourAction*, __fastcall,
    (EnumParkourAction, __m128*, __m128*, __m128*, int, char, __int64, AvailableParkourAction*));
DEFINE_GAME_FUNCTION(ConstructParkourAction_B, 0x1401D1530, AvailableParkourAction*, __fastcall,
    (EnumParkourAction, __m128*, __m128*, __m128*, int, char, __int64, __int64, Entity*, AvailableParkourAction*));
DEFINE_GAME_FUNCTION(AvailableParkourAction__InitializePlayerRef, 0x140159940, void, __fastcall,
    (PlayerRefInParkourAction*, Entity*));

static bool g_Enabled = false;
static int g_ProtectedType = 39;
static float g_HeightMin = -4.0f, g_HeightMax = 2.0f;
static float g_HorizMin = -4.0f, g_HorizMax = 4.0f;
static float g_DistMin = 0.0f, g_DistMax = 8.0f;
static float g_CurveMin = -5.0f, g_CurveMax = 5.0f;

static void WidenAction(AvailableParkourAction* act)
{
    act->heightDifferenceMin = g_HeightMin;
    act->heightDifferenceMax = g_HeightMax;
    act->horizontalDifferenceMin = g_HorizMin;
    act->horizontalDifferenceMax = g_HorizMax;
    act->distanceMin = g_DistMin;
    act->distanceMax = g_DistMax;
    act->curveAllowedRangeMin = g_CurveMin;
    act->curveAllowedRangeMax = g_CurveMax;
    act->expectedVerticalSpeed_mb = 9.0f;
    act->expectedHorizontalSpeed = 4.0f;
    act->fitness = 9999.0f;
}

static bool CreateParkourAction_A_Replacement(
    EnumParkourAction actionType, __m128* loc, __m128* a3, __m128* dir,
    float a5, int a6, char a7, CollisionProbeForParkour_mb* probe,
    Entity* player, AvailableParkourAction* prevAction,
    AvailableParkourAction** outAction, float a12, float eps)
{
    AvailableParkourAction* act = ConstructParkourAction_A(actionType, loc, a3, dir, a6, a7, (uint64)probe, prevAction);
    *outAction = act;
    if (!act) return false;
    if (player) AvailableParkourAction__InitializePlayerRef(&act->playerRef, player);
    GET_AND_CAST_FANCY_FUNC(*act, ParkourActionKnownFancyVFuncs::Set2FloatsAfterCreation)(act, a12, eps);
    if (g_Enabled && (int)actionType == g_ProtectedType)
        WidenAction(act);
    bool fits = GET_AND_CAST_FANCY_FUNC(*act, ParkourActionKnownFancyVFuncs::InitialTestIfActionFits)(act, loc, a3, dir, a5, a6, (uint64)probe, player, prevAction);
    if (!fits) {
        act->Unk008_Destroy(0);
        ACU::Memory::ACUDeallocateBytes((byte*)act);
        *outAction = nullptr;
        return false;
    }
    return true;
}

static bool CreateParkourAction_B_Replacement(
    EnumParkourAction actionType, __m128* loc, __m128* a3, __m128* dir,
    float a5, int a6, char a7, uint64 a8, uint64 a9,
    Entity* player, AvailableParkourAction* prevAction,
    AvailableParkourAction** outAction, float a13, float eps)
{
    AvailableParkourAction* act = ConstructParkourAction_B(actionType, loc, a3, dir, a6, a7, a8, a9, player, prevAction);
    *outAction = act;
    if (!act) return false;
    if (player) AvailableParkourAction__InitializePlayerRef(&act->playerRef, player);
    GET_AND_CAST_FANCY_FUNC(*act, ParkourActionKnownFancyVFuncs::Set2FloatsAfterCreation)(act, a13, eps);
    if (g_Enabled && (int)actionType == g_ProtectedType)
        WidenAction(act);
    bool fits = GET_AND_CAST_FANCY_FUNC(*act, ParkourActionKnownFancyVFuncs::InitialTestIfActionFits)(act, loc, a3, dir, a5, a6, a9, player, prevAction);
    if (!fits) {
        act->Unk008_Destroy(0);
        ACU::Memory::ACUDeallocateBytes((byte*)act);
        *outAction = nullptr;
        return false;
    }
    return true;
}

ParkourCreationHook::ParkourCreationHook()
{
    PresetScript_ReplaceFunctionAtItsStart(0x1401D1260, CreateParkourAction_A_Replacement);
    PresetScript_ReplaceFunctionAtItsStart(0x1401D13C0, CreateParkourAction_B_Replacement);
}

void NoEarlyDiscardPlugin::OnBeforeActivate()
{
}

void NoEarlyDiscardPlugin::OnBeforeDeactivate()
{
    if (m_HookActive)
    {
        m_CreationHook.Deactivate();
        m_HookActive = false;
    }
    g_Enabled = false;
}

void NoEarlyDiscardPlugin::OnUpdate()
{
    bool needHook = m_Enabled;
    if (needHook && !m_HookActive)
    {
        m_CreationHook.Activate();
        m_HookActive = true;
    }
    else if (!needHook && m_HookActive)
    {
        m_CreationHook.Deactivate();
        m_HookActive = false;
    }
    g_Enabled = m_Enabled;
    g_HeightMin = m_HeightMin;
    g_HeightMax = m_HeightMax;
    g_HorizMin = m_HorizMin;
    g_HorizMax = m_HorizMax;
    g_DistMin = m_DistMin;
    g_DistMax = m_DistMax;
    g_CurveMin = m_CurveMin;
    g_CurveMax = m_CurveMax;
}

void NoEarlyDiscardPlugin::OnImGuiRender()
{
    ImGui::Text("No Early Discard");
    ImGui::SeparatorText("Actions");
    ImGui::Checkbox("climbFacade_sidehop (39)", &m_Protect39);
    ImGui::SeparatorText("Protection Settings");
    ImGui::Checkbox("Enabled", &m_Enabled);
    ImGui::SameLine();
    ImGui::TextColored(m_HookActive ? ImVec4(0,1,0,1) : ImVec4(1,0.55f,0,1),
                       m_HookActive ? "HOOK ACTIVE" : "HOOK INACTIVE");
    if (ImGui::CollapsingHeader("Range Values", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto slider = [](const char* label, float* v, float lo, float hi) {
            ImGui::SetNextItemWidth(100);
            ImGui::DragFloat(label, v, 0.1f, lo, hi, "%.1f");
        };
        slider("Height Min ", &m_HeightMin, -20, 20);
        slider("Height Max ", &m_HeightMax, -20, 20);
        slider("Horiz Min  ", &m_HorizMin, -20, 20);
        slider("Horiz Max  ", &m_HorizMax, -20, 20);
        slider("Dist Min   ", &m_DistMin, 0, 20);
        slider("Dist Max   ", &m_DistMax, 0, 20);
        slider("Curve Min  ", &m_CurveMin, -20, 20);
        slider("Curve Max  ", &m_CurveMax, -20, 20);
    }
}
