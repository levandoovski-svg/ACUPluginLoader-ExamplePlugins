#include "pch.h"
#include "PreventAutoDrawPlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/Entity.h"
#include "ACU/ACUGetSingletons.h"

static bool g_PreventDrawEnabled = false;

DEFINE_GAME_FUNCTION(onEnterFight_canDisableUnsheathing_P, 0x1426582C0,
    __int64, __fastcall, (__int64 a1, __int64 a2, char a3));

DEFINE_GAME_FUNCTION(ReattachWeaponToSheathOrHolster, 0x141B05570,
    void, __fastcall, (__int64 a1, int p_1melee2ranged));

static void OnCloseRangeUnsheatheDecision(AllRegisters* params)
{
    if (g_PreventDrawEnabled)
    {
        *params->rax_ = 0;
        return;
    }
    *params->rax_ = onEnterFight_canDisableUnsheathing_P(
        params->rbp_, params->rsi_, params->r8_);
}

struct PreventAutoDrawHook : AutoAssemblerCodeHolder_Base
{
    PreventAutoDrawHook()
    {
        PresetScript_CCodeInTheMiddle(
            0x14265D16B, 5,
            OnCloseRangeUnsheatheDecision,
            0x14265D170,
            false);
    }
};

static void InstallHook()
{
    static AutoAssembleWrapper<PreventAutoDrawHook> wrapper;
    wrapper.Activate();
}

void PreventAutoDrawPlugin::DoForceSheathe()
{
    Entity* player = ACU::GetPlayer();
    if (!player) return;
    __try {
        ReattachWeaponToSheathOrHolster((__int64)player, 1);
        ReattachWeaponToSheathOrHolster((__int64)player, 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void PreventAutoDrawPlugin::LoadSettings()
{
    InstallHook();
}

void PreventAutoDrawPlugin::OnUpdate()
{
    g_PreventDrawEnabled = m_Enabled;
    if (m_Enabled)
        DoForceSheathe();
}

void PreventAutoDrawPlugin::OnImGuiRender()
{
    ImGui::Checkbox("Prevent Auto Draw", &m_Enabled);
}
