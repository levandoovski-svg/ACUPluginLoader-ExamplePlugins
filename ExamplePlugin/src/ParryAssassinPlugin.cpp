#include "pch.h"
#include "ParryAssassinPlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include <shlobj.h>
#include <mmsystem.h> // PlaySoundW + SND_* flags (not pulled in via windows.h because of WIN32_LEAN_AND_MEAN)

#pragma comment(lib, "winmm.lib")

bool g_ParryAssaultEnabled = false;
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
    if ((g_ParryAssaultEnabled || g_SmokeBombAssaultEnabled) && !IsPlayerInActiveCombat())
    {
        *params->rax_ = 0;
        return;
    }
    *params->rax_ = WhenDecidingIfAssassinationShouldBeDisallowed_Stage1ChaseVersion(
        params->rcx_, (SharedPtr_mb*)params->rdx_, (SharedPtr_mb*)params->r8_);
}

static void OnAssassinationDecisionStage2(AllRegisters* params)
{
    if ((g_ParryAssaultEnabled || g_SmokeBombAssaultEnabled) && !IsPlayerInActiveCombat())
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

// ---- Insta-critical kill after two parries (ACU 1.5.0, sites found in ACUFixes "Cheats") ----
// 0x1409E76FB (12 bytes): mov eax, 1.0f; mov [rbx+34h],eax; mov [rbx+38h],eax; nop
//   -> forces the melee damage multiplier to 100% ("Increase additional damage to 100%").
// 0x1405769EB (5 bytes): movss xmm6,[rax+50h] -> movss xmm6,xmm8 (paired rewrite on the same path).
struct CritDamagePatch : AutoAssemblerCodeHolder_Base
{
    CritDamagePatch()
    {
        DEFINE_ADDR(critDamageMul, 0x1409E76FB);
        critDamageMul = { "B8 00 00 80 3F", "89 43 34", "89 43 38", "90" };

        DEFINE_ADDR(critDamageLoad, 0x1405769EB);
        critDamageLoad = { "F3 41 0F 10 F0" };
    }
};

// 0x140704188 (2 bytes): jg +0x0D -> nop nop
//   -> "Unrestricted Critical Kill": removes the gate restricting when a critical kill may occur.
struct UnrestrictedCritKillPatch : AutoAssemblerCodeHolder_Base
{
    UnrestrictedCritKillPatch()
    {
        DEFINE_ADDR(critKillGate, 0x140704188);
        critKillGate = { "90 90" };
    }
};

static AutoAssembleWrapper<CritDamagePatch>& GetCritDamagePatch()
{
    static AutoAssembleWrapper<CritDamagePatch> w;
    return w;
}

static AutoAssembleWrapper<UnrestrictedCritKillPatch>& GetCritKillPatch()
{
    static AutoAssembleWrapper<UnrestrictedCritKillPatch> w;
    return w;
}

void ParryAssassinPlugin::UpdateCritPatches()
{
    bool wantDmg  = m_Enabled && m_CritKillEnabled && m_BoostCritDamage && m_ReadyToAssassinate;
    bool wantKill = m_Enabled && m_CritKillEnabled && m_UnrestrictedCritEnabled && m_ReadyToAssassinate;

    if (wantDmg)  GetCritDamagePatch().Activate(); else GetCritDamagePatch().Deactivate();
    if (wantKill) GetCritKillPatch().Activate();   else GetCritKillPatch().Deactivate();

    m_PatchesActive = wantDmg || wantKill;
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

static constexpr const char* kParryAssassinIniSection = "ParryAssassin";
static constexpr const char* kParryAssassinIniFileName = "ParryAssassinPlugin.ini";

static std::string GetParryAssassinIniPath()
{
    char docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\" + kParryAssassinIniFileName;
    return kParryAssassinIniFileName;
}

static bool EnsureParryAssassinIniDirectoryExists()
{
    const fs::path directory = fs::path(GetParryAssassinIniPath()).parent_path();
    if (directory.empty())
        return true;

    std::error_code error;
    fs::create_directories(directory, error);
    return !error;
}

void ParryAssassinPlugin::LoadSettings()
{
    InstallAssassinationHooks();

    const std::string iniPath = GetParryAssassinIniPath();
    m_Enabled = GetPrivateProfileIntA(
        kParryAssassinIniSection, "Enabled", m_Enabled ? 1 : 0, iniPath.c_str()) != 0;
    m_CritKillEnabled = GetPrivateProfileIntA(
        kParryAssassinIniSection, "CritKillEnabled", m_CritKillEnabled ? 1 : 0, iniPath.c_str()) != 0;
    m_BoostCritDamage = GetPrivateProfileIntA(
        kParryAssassinIniSection, "BoostCritDamage", m_BoostCritDamage ? 1 : 0, iniPath.c_str()) != 0;
    m_UnrestrictedCritEnabled = GetPrivateProfileIntA(
        kParryAssassinIniSection, "UnrestrictedCritEnabled", m_UnrestrictedCritEnabled ? 1 : 0, iniPath.c_str()) != 0;

    // This also creates the file (and its directory) on first run and adds any
    // settings that are missing from an older INI file.
    SaveSettings();
    UpdateCritPatches();
}

void ParryAssassinPlugin::SaveSettings()
{
    if (!EnsureParryAssassinIniDirectoryExists())
        return;

    const std::string iniPath = GetParryAssassinIniPath();
    WritePrivateProfileStringA(
        kParryAssassinIniSection, "Enabled", m_Enabled ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA(
        kParryAssassinIniSection, "CritKillEnabled", m_CritKillEnabled ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA(
        kParryAssassinIniSection, "BoostCritDamage", m_BoostCritDamage ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA(
        kParryAssassinIniSection, "UnrestrictedCritEnabled", m_UnrestrictedCritEnabled ? "1" : "0", iniPath.c_str());
}

void ParryAssassinPlugin::OnUpdate()
{
    if (!m_Enabled)
    {
        UpdateCritPatches(); // restores original bytes if the plugin is switched off mid-window
        return;
    }

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

    // Apply the insta-critical-kill patches on the rising edge of "ready",
    // and restore the original bytes when the ready window expires.
    UpdateCritPatches();
}

void ParryAssassinPlugin::OnImGuiRender()
{
    if (ImGui::Checkbox("Parry Assassination", &m_Enabled))
    {
        UpdateCritPatches();
        SaveSettings();
    }

    if (m_Enabled)
    {
        if (ImGui::Checkbox("Insta-critical kill after two parries", &m_CritKillEnabled))
        {
            UpdateCritPatches();
            SaveSettings();
        }

        if (m_CritKillEnabled)
        {
            if (ImGui::Checkbox("Max critical damage (100%)", &m_BoostCritDamage))
            {
                UpdateCritPatches();
                SaveSettings();
            }
            if (ImGui::Checkbox("Unrestricted critical kill (bypass gate)", &m_UnrestrictedCritEnabled))
            {
                UpdateCritPatches();
                SaveSettings();
            }
            ImGui::TextDisabled("Patches apply while the ready window is open.");
        }
    }
}
