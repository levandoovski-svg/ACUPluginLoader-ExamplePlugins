#include "pch.h"
#include "ParryAssassinPlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/BhvAssassin.h"
#include "ACU/WhenHighlightedNPCChanges.h"
#include "ACU/SharedPtr.h"
#include "ACU/Entity.h"
#include "ImGuiCTX.h"
#include <shlobj.h>
#include <cstdlib>  // strtof
#include <cstdio>   // snprintf
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

// ---- Gold-highlight visual cue (ACU 1.5.0, addresses verified against the runtime dump in MODLOG/scripts) ----
//
// The vanilla enemy-highlight FX exposes named parameters "critical color" (Vector4)
// and "critical state" (float). Their ids are resolved once at startup and cached at
// 0x145197D74 ("critical color") and 0x145197D78 ("critical state")
// (see the dynamic-initializer stubs at 0x142D8E850/0x142D8E870, which look the names
// up via 0x142759CF0).
//
// FUN_14087f850 applies the current highlight params to the highlighted NPC's FX instance:
//   0x14087F9A6  mov rax,[rsi]            ; SharedBlock* of the FX instance
//   0x14087F9A9  mov [rsp+0x58],rax       ; (refcounted copy for the call)
//   0x14087F9B2  mov r9,[rsp+0xB8]        ; <- hook A: color source for "critical color"
//   0x14087F9BA  mov r8d,[0x145197D74]    ; "critical color" param id
//   0x14087F9C1  lea rdx,[rsp+0x58]
//   0x14087F9C6  mov rcx,rbp
//   0x14087F9C9  call 0x141E30220         ; SetParamVector(ctx, &fxShared, id, colorPtr)
//   0x14087F9CE  mov rax,[rsi]
//   0x14087F9D1  mov [rsp+0x60],rax
//   0x14087F9DA  mov r8d,[0x145197D78]    ; "critical state" param id
//   0x14087F9E1  movss xmm3,[rsp+0xC0]    ; <- hook B: state value (vanilla passes 0.0 or 1.0)
//   0x14087F9EA  lea rdx,[rsp+0x60]
//   0x14087F9EF  mov rcx,rbp
//   0x14087F9F2  call 0x141E302D0         ; SetParamFloat(ctx, &fxShared, id, value)
//
// Both hooks are pass-through unless the parry-ready window is open. While it is,
// the color fed to the highlight FX of the player's currently highlighted NPC
// (identity-checked against WhenHighlightedNPCChanges::highlightedNPCFxInstance)
// is replaced with the configured gold, and the "critical state" is forced to 1.0.
// When the window closes the game re-applies its own values on the next update.

static bool  g_HighlightCueEnabled = false;
static bool  g_ForceCriticalState  = true;
static bool  g_StrictTargetCheck   = true;
static float g_GoldHighlightColor[4] = { 1.0f, 0.84f, 0.0f, 0.0f };

// SharedBlock::GetPtr() semantics (see ACU/SharedPtr.h), applied defensively.
static void* SharedBlock_GetPtr(void* sharedBlock)
{
    if (!sharedBlock) { return nullptr; }
    SharedBlock& sb = *static_cast<SharedBlock*>(sharedBlock);
    return sb.GetPtr();
}

static bool HighlightCueShouldApply(const void* fxSharedBlock)
{
    if (!g_HighlightCueEnabled || !g_ParryAssaultEnabled) { return false; }
    if (!fxSharedBlock) { return false; }
    if (!g_StrictTargetCheck) { return true; }

    // Identity check: only tint the FX of the NPC the player is currently attacking.
    BhvAssassin* bhv = ACU::GetPlayerBhvAssassin();
    if (!bhv || !bhv->toHighlightedNPC) { return false; }
    const void* playerSlot = bhv->toHighlightedNPC->highlightedNPCFxInstance;
    if (!playerSlot) { return false; }
    if (playerSlot == fxSharedBlock) { return true; }

    void* playerFx = SharedBlock_GetPtr(const_cast<void*>(playerSlot));
    void* hookFx   = SharedBlock_GetPtr(const_cast<void*>(fxSharedBlock));
    return playerFx && playerFx == hookFx;
}

// Stolen instruction: mov r9,[rsp+0xB8] (color source for the "critical color" param).
static void OnHighlightCriticalColor(AllRegisters* params)
{
    void* colorPtr = *(void**)(params->GetRSP() + 0xB8);
    const void* fxShared = *(void* const*)(params->GetRSP() + 0x58);
    if (HighlightCueShouldApply(fxShared))
    {
        colorPtr = &g_GoldHighlightColor;
    }
    params->r9_ = reinterpret_cast<unsigned long long>(colorPtr);
}

// Stolen instruction: movss xmm3,[rsp+0xC0] (value for the "critical state" param).
static void OnHighlightCriticalState(AllRegisters* params)
{
    float value = *(float*)(params->GetRSP() + 0xC0);
    const void* fxShared = *(void* const*)(params->GetRSP() + 0x60);
    if (g_ForceCriticalState && HighlightCueShouldApply(fxShared))
    {
        value = 1.0f;
    }
    params->XMM3.fa[0] = value;
}

struct HighlightCueHooks : AutoAssemblerCodeHolder_Base
{
    HighlightCueHooks()
    {
        PresetScript_CCodeInTheMiddle(
            0x14087F9B2, 8, OnHighlightCriticalColor,
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, false);
        PresetScript_CCodeInTheMiddle(
            0x14087F9E1, 9, OnHighlightCriticalState,
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, false);
    }
};

static void InstallHighlightCueHooks()
{
    static AutoAssembleWrapper<HighlightCueHooks> w;
    w.Activate();
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

void ParryAssassinPlugin::SyncHighlightCueGlobals()
{
    // Effective flag also requires the plugin itself to be enabled; synced here and
    // at the top of OnUpdate so the cue can never outlive the plugin toggle.
    g_HighlightCueEnabled = m_Enabled && m_HighlightCueEnabled;
    g_ForceCriticalState  = m_ForceCriticalState;
    g_StrictTargetCheck   = m_StrictTargetCheck;
    std::copy(std::begin(m_GoldHighlightColor), std::end(m_GoldHighlightColor),
              std::begin(g_GoldHighlightColor));
}

static void ParseColorIni(const char* str, float out[4])
{
    const float defaults[4] = { 1.0f, 0.84f, 0.0f, 0.0f };
    const char* cursor = str ? str : "";
    for (int i = 0; i < 4; ++i)
    {
        char* end = nullptr;
        float value = strtof(cursor, &end);
        out[i] = (end != cursor) ? value : defaults[i];
        cursor = end;
        while (*cursor && (*cursor == ',' || *cursor == ' ')) { ++cursor; }
    }
}

static std::string ColorToIni(const float color[4])
{
    char buf[64] = {};
    snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f",
             color[0], color[1], color[2], color[3]);
    return buf;
}

void ParryAssassinPlugin::LoadSettings()
{
    InstallAssassinationHooks();
    InstallHighlightCueHooks();

    const std::string iniPath = GetParryAssassinIniPath();
    m_Enabled = GetPrivateProfileIntA(
        kParryAssassinIniSection, "Enabled", m_Enabled ? 1 : 0, iniPath.c_str()) != 0;
    m_CritKillEnabled = GetPrivateProfileIntA(
        kParryAssassinIniSection, "CritKillEnabled", m_CritKillEnabled ? 1 : 0, iniPath.c_str()) != 0;
    m_BoostCritDamage = GetPrivateProfileIntA(
        kParryAssassinIniSection, "BoostCritDamage", m_BoostCritDamage ? 1 : 0, iniPath.c_str()) != 0;
    m_UnrestrictedCritEnabled = GetPrivateProfileIntA(
        kParryAssassinIniSection, "UnrestrictedCritEnabled", m_UnrestrictedCritEnabled ? 1 : 0, iniPath.c_str()) != 0;
    m_HighlightCueEnabled = GetPrivateProfileIntA(
        kParryAssassinIniSection, "HighlightCueEnabled", m_HighlightCueEnabled ? 1 : 0, iniPath.c_str()) != 0;
    m_ForceCriticalState = GetPrivateProfileIntA(
        kParryAssassinIniSection, "ForceCriticalState", m_ForceCriticalState ? 1 : 0, iniPath.c_str()) != 0;
    m_StrictTargetCheck = GetPrivateProfileIntA(
        kParryAssassinIniSection, "StrictTargetCheck", m_StrictTargetCheck ? 1 : 0, iniPath.c_str()) != 0;
    {
        char colorBuf[128] = {};
        GetPrivateProfileStringA(
            kParryAssassinIniSection, "GoldHighlightColor",
            ColorToIni(m_GoldHighlightColor).c_str(), colorBuf, sizeof(colorBuf), iniPath.c_str());
        ParseColorIni(colorBuf, m_GoldHighlightColor);
    }

    // This also creates the file (and its directory) on first run and adds any
    // settings that are missing from an older INI file.
    SaveSettings();
    UpdateCritPatches();
    SyncHighlightCueGlobals();
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
    WritePrivateProfileStringA(
        kParryAssassinIniSection, "HighlightCueEnabled", m_HighlightCueEnabled ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA(
        kParryAssassinIniSection, "ForceCriticalState", m_ForceCriticalState ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA(
        kParryAssassinIniSection, "StrictTargetCheck", m_StrictTargetCheck ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA(
        kParryAssassinIniSection, "GoldHighlightColor", ColorToIni(m_GoldHighlightColor).c_str(), iniPath.c_str());
}

void ParryAssassinPlugin::OnUpdate()
{
    SyncHighlightCueGlobals();

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
        SyncHighlightCueGlobals();
        SaveSettings();
    }

    if (m_Enabled)
    {
        if (ImGui::Checkbox("Insta-critical kill after two parries", &m_CritKillEnabled))
        {
            UpdateCritPatches();
            SaveSettings();
        }

        if (ImGui::Checkbox("Gold highlight cue while ready", &m_HighlightCueEnabled))
        {
            SyncHighlightCueGlobals();
            SaveSettings();
        }
        if (m_HighlightCueEnabled)
        {
            ImGuiCTX::Indent _indent;
            if (ImGui::ColorEdit4("Gold color", m_GoldHighlightColor, ImGuiColorEditFlags_NoInputs))
            {
                SyncHighlightCueGlobals();
                SaveSettings();
            }
            if (ImGui::Checkbox("Force \"critical state\" param to 1.0", &m_ForceCriticalState))
            {
                SyncHighlightCueGlobals();
                SaveSettings();
            }
            if (ImGui::Checkbox("Strict target check (only the highlighted NPC)", &m_StrictTargetCheck))
            {
                SyncHighlightCueGlobals();
                SaveSettings();
            }
            ImGui::TextDisabled("Tints the highlight FX of the enemy you are attacking\nwhile the ready window is open.");
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
