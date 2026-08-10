#include "pch.h"
#include "ParryAssassinPlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include <shlobj.h>

#pragma comment(lib, "winmm.lib")

bool g_ParryAssaultEnabled = false;
bool g_ParryAssaultTriggerOverride = false;
extern bool g_SmokeBombAssaultEnabled;

static bool IsPlayerInActiveCombat()
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) return false;
    for (auto& r : hs->primaryCallbackReceivers)
    {
        if (!r.pNode) continue;
        uint64_t addr = (uint64_t)r.pNode->Enter;
        if (addr == 0x1419BAA40 || addr == 0x1419AA430 ||
            addr == 0x1419ABA10 || addr == 0x1419AB450 ||
            addr == 0x1419AAD80 || addr == 0x1419AAA90 ||
            addr == 0x1419A98D0 || addr == 0x141999D30 ||
            addr == 0x1419B39D0 || addr == 0x1419B3460 ||
            addr == 0x1419AF160 || addr == 0x1419AEA00)
            return true;
    }
    return false;
}

class SharedPtr_mb;

DEFINE_GAME_FUNCTION(WhenDecidingIfAssassinationShouldBeDisallowed_Stage1ChaseVersion, 0x1404E90C0,
    char, __fastcall, (__int64 a1, SharedPtr_mb* a2, SharedPtr_mb* a3));
DEFINE_GAME_FUNCTION(WhenDecidingIfAssassinationShouldBeDisallowed_Stage2ChaseVersion, 0x1404E9310,
    char, __fastcall, (__int64 a1, SharedPtr_mb* a2, SharedPtr_mb* a3));

// Force-sheathe after ready (skip slow sheathe animation before assassination)
DEFINE_GAME_FUNCTION(ReattachWeaponToSheathOrHolster, 0x141B05570,
    void, __fastcall, (__int64 a1, int p_1melee2ranged));

static void OnAssassinationDecisionStage1(AllRegisters* params)
{
    if ((g_ParryAssaultEnabled || g_SmokeBombAssaultEnabled) && (!IsPlayerInActiveCombat() || g_ParryAssaultTriggerOverride))
    {
        *params->rax_ = 0;
        return;
    }
    *params->rax_ = WhenDecidingIfAssassinationShouldBeDisallowed_Stage1ChaseVersion(
        params->rcx_, (SharedPtr_mb*)params->rdx_, (SharedPtr_mb*)params->r8_);
}

static void OnAssassinationDecisionStage2(AllRegisters* params)
{
    if ((g_ParryAssaultEnabled || g_SmokeBombAssaultEnabled) && (!IsPlayerInActiveCombat() || g_ParryAssaultTriggerOverride))
    {
        *params->rax_ = 0;
        return;
    }
    *params->rax_ = WhenDecidingIfAssassinationShouldBeDisallowed_Stage2ChaseVersion(
        params->rcx_, (SharedPtr_mb*)params->rdx_, (SharedPtr_mb*)params->r8_);
}

struct AssassinateHook1 : AutoAssemblerCodeHolder_Base
{
    AssassinateHook1()
    {
        PresetScript_CCodeInTheMiddle(
            0x140CD9BED, 5, OnAssassinationDecisionStage1,
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, false);
    }
};

struct AssassinateHook2 : AutoAssemblerCodeHolder_Base
{
    AssassinateHook2()
    {
        PresetScript_CCodeInTheMiddle(
            0x140CD9C95, 5, OnAssassinationDecisionStage2,
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, false);
    }
};

static void InstallAssassinationHooks()
{
    static AutoAssembleWrapper<AssassinateHook1> w1;
    static AutoAssembleWrapper<AssassinateHook2> w2;
    w1.Activate();
    w2.Activate();
}

static float GetTimeSeconds()
{
    return static_cast<float>(GetTickCount64()) / 1000.0f;
}

bool ParryAssassinPlugin::IsParryActive()
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) return false;
    for (auto& r : hs->primaryCallbackReceivers)
        if (r.pNode && (uint64_t)r.pNode->Enter == 0x1419AAA90)
            return true;
    return false;
}

static void PlayReadySound()
{
    char docs[MAX_PATH];
    std::string fullPath;
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        fullPath = std::string(docs) + "\\Assassin's Creed Unity\\parry_ready.wav";
    else
        fullPath = "parry_ready.wav";

    int len = MultiByteToWideChar(CP_UTF8, 0, fullPath.c_str(), -1, nullptr, 0);
    if (len <= 0) return;
    std::wstring wpath(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, fullPath.c_str(), -1, &wpath[0], len);
    PlaySoundW(wpath.c_str(), NULL, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);
}

static void ForceSheatheWeapon()
{
    Entity* player = ACU::GetPlayer();
    if (!player) return;
    __try {
        ReattachWeaponToSheathOrHolster((__int64)player, 0);
        ReattachWeaponToSheathOrHolster((__int64)player, 1);
        ReattachWeaponToSheathOrHolster((__int64)player, 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SimulateAssassinateClick()
{
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

void ParryAssassinPlugin::LoadSettings()
{
    InstallAssassinationHooks();
}

void ParryAssassinPlugin::OnUpdate()
{
    if (!m_Enabled) return;

    bool parryNow   = IsParryActive();
    bool risingEdge = parryNow && !m_PrevParryState;
    m_PrevParryState = parryNow;

    if (risingEdge)
    {
        float now = GetTimeSeconds();
        if ((now - m_LastParryTime) > 4.0f)
        {
            m_ParryCount         = 1;
            m_ReadyToAssassinate = false;
        }
        else
        {
            m_ParryCount++;
        }
        m_LastParryTime = now;
        if (m_ParryCount >= 2)
            m_ReadyToAssassinate = true;
    }

    // Sound on rising edge
    bool readyRising = m_ReadyToAssassinate && !m_PrevReady;
    m_PrevReady = m_ReadyToAssassinate;
    if (readyRising)
        PlayReadySound();

    // When ready (within 2s window), detect if player starts sheathe anim and skip it
    if (m_ReadyToAssassinate &&
        (GetTimeSeconds() - m_LastParryTime) < 10.0f)
    {
        HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
        if (hs)
            for (auto& r : hs->primaryCallbackReceivers)
                if (r.pNode && (uint64_t)r.pNode->Enter == 0x141B02AF0)
                    ForceSheatheWeapon();
    }

    if (m_ReadyToAssassinate &&
        (GetTimeSeconds() - m_LastParryTime) > 8.0f)
    {
        m_ReadyToAssassinate = false;
        m_ParryCount         = 0;
    }

    g_ParryAssaultEnabled = m_ReadyToAssassinate;

    // Brute force trigger: rising edge → override + sheathe + click in one frame
    float now = GetTimeSeconds();
    bool keyDown = (GetAsyncKeyState(m_TriggerKey) & 0x8000) != 0;
    if (keyDown && !m_TriggerKeyPrev)
    {
        g_ParryAssaultTriggerOverride = true;
        m_OverrideStartTime = now;
        ForceSheatheWeapon();
        SimulateAssassinateClick();
    }
    m_TriggerKeyPrev = keyDown;

    // Clear override after 0.5s
    if (g_ParryAssaultTriggerOverride && (now - m_OverrideStartTime) > 0.5f)
        g_ParryAssaultTriggerOverride = false;
}

void ParryAssassinPlugin::OnImGuiRender()
{
    ImGui::Checkbox("Parry Assassination", &m_Enabled);

    if (m_Enabled)
    {
        char keyLabel[96];
        if (m_TriggerKey >= 'A' && m_TriggerKey <= 'Z')
            snprintf(keyLabel, sizeof(keyLabel), "Assassinate Trigger Key: %c", (char)m_TriggerKey);
        else
        {
            const char* name = "Unknown";
            if      (m_TriggerKey == VK_SPACE)    name = "Space";
            else if (m_TriggerKey == VK_LSHIFT)   name = "LShift";
            else if (m_TriggerKey == VK_LCONTROL) name = "LCtrl";
            snprintf(keyLabel, sizeof(keyLabel), "Assassinate Trigger Key: %s", name);
        }

        if (m_WaitingForKeybind)
            ImGui::Text("Press a key for trigger...");
        else if (ImGui::Button(keyLabel, ImVec2(250, 0)))
            m_WaitingForKeybind = true;

        if (m_WaitingForKeybind)
        {
            for (int vk = 0x08; vk <= 0xFE; ++vk)
            {
                if (GetAsyncKeyState(vk) & 0x8000)
                {
                    m_TriggerKey = vk;
                    m_WaitingForKeybind = false;
                    break;
                }
            }
        }
    }
}
