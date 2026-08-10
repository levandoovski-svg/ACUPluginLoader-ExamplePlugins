#include "pch.h"
#include "DesyncKeybindPlugin.h"

#include "ACU/ACUGetSingletons.h"
#include "ACU/CSrvPlayerHealth.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/Entity.h"
#include "ACU/BhvAssassin.h"
#include "ACU/WhenHighlightedNPCChanges.h"

#include <shlobj.h>
#include <cstdio>
#include <string>

static constexpr int kKeyRangeMin = 0x01;
static constexpr int kKeyRangeMax = 0xFE;

// ---------------------------------------------------------------------------
// Game-memory access -- POD-only inside __try, exactly like
// AutoDesyncOnAssassinatePlugin. No C++ objects with destructors may be
// declared inside the guarded functions (MSVC error C2712 otherwise).
// ---------------------------------------------------------------------------

static bool TriggerDesync_raw()
{
    CSrvPlayerHealth* health = ACU::GetPlayerHealth();
    if (!health) return false;
    health->isDesynchronizationNow = true;
    return true;
}

static bool SafeTriggerDesync()
{
    __try
    {
        return TriggerDesync_raw();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Pointer chain resolved to something invalid this frame - drop it,
        // don't crash the process over a missed desync.
        return false;
    }
}

struct ChainResultPOD
{
    bool cameraComponentValid;
    bool playerEntityValid;
    bool bhvAssassinValid;
    bool healthValid;
};

static void CheckChain_raw(ChainResultPOD& out)
{
    out.cameraComponentValid = (ACU::GetPlayerCameraComponent() != nullptr);
    out.playerEntityValid    = (ACU::GetPlayer() != nullptr);
    out.bhvAssassinValid     = (ACU::GetPlayerBhvAssassin() != nullptr);
    out.healthValid          = (ACU::GetPlayerHealth() != nullptr);
}

static void SafeCheckChain(ChainResultPOD& out)
{
    __try
    {
        CheckChain_raw(out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out.cameraComponentValid = false;
        out.playerEntityValid    = false;
        out.bhvAssassinValid     = false;
        out.healthValid          = false;
    }
}

// ---------------------------------------------------------------------------
// Assassination human-state detection -- whole-tree walk, zero code hooks.
//
// Same per-frame polling idea as AllowSmokeAssassinatePlugin, but for the
// assassination nodes.  Assassination_PP / Assassination_P are PARENT nodes,
// so a leaf-only scan of primaryCallbackReceivers never matches them; the
// whole FunctorBase tree is walked from the holder root instead (depth cap
// 64 + node budget 512 so a mutated/cyclic tree can't loop forever).
//
// Tracked Enter addresses (from the live human-state log, this build):
//   0x141A4A4B0  Assassination_PP
//   0x141A462F0  Assassination_P
//   0x141A40BE0  Assassination_SecondHalf_mb
// ---------------------------------------------------------------------------

static const uint64_t kAssassinationEnterStates[] = {
    0x141A4A4B0ULL,  // Assassination_PP
    0x141A462F0ULL,  // Assassination_P
    0x141A40BE0ULL,  // Assassination_SecondHalf_mb
};

static bool IsKnownAssassinationEnter(uint64_t enter)
{
    for (uint64_t e : kAssassinationEnterStates)
        if (enter == e)
            return true;
    return false;
}

static bool WalkStateTree_raw(FunctorBase* node, int depth, int& budget)
{
    if (!node || depth > 64 || budget <= 0)
        return false;
    --budget;
    if (IsKnownAssassinationEnter((uint64_t)node->Enter))
        return true;
    if (node->directChild_mb &&
        WalkStateTree_raw(node->directChild_mb, depth + 1, budget))
        return true;
    for (FunctorBase* child : node->nonoverridingChildren)
        if (child && WalkStateTree_raw(child, depth + 1, budget))
            return true;
    return false;
}

static bool IsInAssassinationState_raw()
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) return false;
    FunctorBase* root = (FunctorBase*)hs; // the holder IS the tree root
    int budget = 512;
    return WalkStateTree_raw(root, 0, budget);
}

static bool SafeIsInAssassinationState()
{
    __try
    {
        return IsInAssassinationState_raw();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Stale pointer in the tree this frame - treat as "not in state",
        // skip the frame, never crash the process.
        return false;
    }
}

// ---------------------------------------------------------------------------
// Highlighted-NPC class lookup -- EntityDescriptor_ bitfields at Entity+0xD4.
// SubDescriptorType is the NPC class (EntityDescriptorNPCSubType); the
// "certain targets" classes are Target(6) and UniqueNPC(33).
// ---------------------------------------------------------------------------

struct VictimResultPOD
{
    bool   victimValid;
    uint32 descriptorType;
    uint32 subDescriptorType;
    uint32 explicitProperty;
};

static void GetVictim_raw(VictimResultPOD& out)
{
    out.victimValid       = false;
    out.descriptorType    = 0;
    out.subDescriptorType = 0;
    out.explicitProperty  = 0;

    BhvAssassin* bhv = ACU::GetPlayerBhvAssassin();
    if (!bhv) return;
    WhenHighlightedNPCChanges* toHl = bhv->toHighlightedNPC;
    if (!toHl) return;
    SharedPtrNew<Entity>* sp = toHl->highlightedNPC;
    if (!sp) return;
    Entity* victim = sp->GetPtr();
    if (!victim) return;

    out.victimValid       = true;
    out.descriptorType    = victim->EntityDescriptor_.DescriptorType;
    out.subDescriptorType = victim->EntityDescriptor_.SubDescriptorType;
    out.explicitProperty  = victim->EntityDescriptor_.ExplicitProperty;
}

static void SafeGetVictim(VictimResultPOD& out)
{
    __try
    {
        GetVictim_raw(out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out.victimValid       = false;
        out.descriptorType    = 0;
        out.subDescriptorType = 0;
        out.explicitProperty  = 0;
    }
}

// CSV of class ids, e.g. "6,33" or "6, 33". Empty string matches nothing.
static bool IsClassInAllowlist(uint32 sub, const char* csv)
{
    if (!csv) return false;
    const char* p = csv;
    while (*p)
    {
        while (*p && (*p == ' ' || *p == ',')) ++p;
        if (!*p) return false;
        uint32 val = 0;
        while (*p >= '0' && *p <= '9')
        {
            val = val * 10 + (uint32)(*p - '0');
            ++p;
        }
        if (val == sub) return true;
        while (*p && *p != ',') ++p;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Config file
// ---------------------------------------------------------------------------

static std::string GetDocumentsAssassinDir()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        std::string dir(documents);
        dir += "\\Assassin's Creed Unity";
        return dir;
    }
    return std::string();
}

static std::string GetConfigPath()
{
    std::string dir = GetDocumentsAssassinDir();
    if (!dir.empty())
    {
        return dir + "\\DesyncKeybindPlugin.ini";
    }
    return "DesyncKeybindPlugin.ini";
}

void DesyncKeybindPlugin::LoadSettings()
{
    const char* path = GetConfigPath().c_str();
    m_Enabled = GetPrivateProfileIntA("DesyncKeybind", "Enabled", m_Enabled ? 1 : 0, path) != 0;
    int key = GetPrivateProfileIntA("DesyncKeybind", "Key", m_KeyCode, path);
    if (key >= kKeyRangeMin && key <= kKeyRangeMax)
    {
        m_KeyCode = key;
    }
    m_AutoDesyncOnAssassination =
        GetPrivateProfileIntA("DesyncKeybind", "AutoDesyncOnAssassination",
                              m_AutoDesyncOnAssassination ? 1 : 0, path) != 0;
    m_OnlyTargetsAllowed =
        GetPrivateProfileIntA("DesyncKeybind", "OnlyTargetsAllowed",
                              m_OnlyTargetsAllowed ? 1 : 0, path) != 0;
    char cls[sizeof(m_AllowedClasses)];
    if (GetPrivateProfileStringA("DesyncKeybind", "AllowedClasses", m_AllowedClasses,
                                 cls, sizeof(cls), path) > 0)
    {
        sprintf_s(m_AllowedClasses, sizeof(m_AllowedClasses), "%s", cls);
    }
    m_WaitingForKey = false;
}

void DesyncKeybindPlugin::SaveSettings()
{
    // CreateDirectoryA fails harmlessly if the folder already exists.
    CreateDirectoryA(GetDocumentsAssassinDir().c_str(), NULL);

    const char* path = GetConfigPath().c_str();
    char buf[16];
    sprintf_s(buf, "%d", m_Enabled ? 1 : 0);
    WritePrivateProfileStringA("DesyncKeybind", "Enabled", buf, path);
    sprintf_s(buf, "%d", m_KeyCode);
    WritePrivateProfileStringA("DesyncKeybind", "Key", buf, path);
    sprintf_s(buf, "%d", m_AutoDesyncOnAssassination ? 1 : 0);
    WritePrivateProfileStringA("DesyncKeybind", "AutoDesyncOnAssassination", buf, path);
    sprintf_s(buf, "%d", m_OnlyTargetsAllowed ? 1 : 0);
    WritePrivateProfileStringA("DesyncKeybind", "OnlyTargetsAllowed", buf, path);
    WritePrivateProfileStringA("DesyncKeybind", "AllowedClasses", m_AllowedClasses, path);
}

// ---------------------------------------------------------------------------
// Per-frame logic
// ---------------------------------------------------------------------------

void DesyncKeybindPlugin::OnUpdate()
{
    // Debug chain readout refreshes even while disabled, so you can watch the
    // plugin's raw view of the world independent of the hotkey.
    ChainResultPOD chain{};
    SafeCheckChain(chain);
    m_Debug_CameraComponentValid = chain.cameraComponentValid;
    m_Debug_PlayerEntityValid    = chain.playerEntityValid;
    m_Debug_BhvAssassinValid     = chain.bhvAssassinValid;
    m_Debug_HealthValid          = chain.healthValid;

    // Highlighted-NPC class readout: refreshes every frame like the chain
    // readout, and the same-frame snapshot feeds the assassination gate
    // below (the target can change mid-animation, so never reuse a stale one).
    VictimResultPOD victim{};
    SafeGetVictim(victim);
    m_Debug_VictimValid             = victim.victimValid;
    m_Debug_VictimDescriptorType    = victim.descriptorType;
    m_Debug_VictimSubDescriptorType = victim.subDescriptorType;
    m_Debug_VictimExplicitProperty  = victim.explicitProperty;

    // Assassination poll: refreshed every frame (even while the hotkey is
    // disabled) so the debug readout always shows the live state.
    const bool inAssassination = SafeIsInAssassinationState();
    const bool assassinationEdge = inAssassination && !m_PrevInAssassinationState;
    m_PrevInAssassinationState = inAssassination;
    m_Debug_InAssassinationState = inAssassination;

    if (m_WaitingForKey)
    {
        for (int vk = kKeyRangeMin; vk <= kKeyRangeMax; ++vk)
        {
            if ((GetAsyncKeyState(vk) & 1) != 0)
            {
                if (vk == VK_ESCAPE)
                {
                    // Cancel without changing the binding.
                    m_WaitingForKey = false;
                }
                else
                {
                    m_KeyCode = vk;
                    m_WaitingForKey = false;
                    SaveSettings();
                }
                break;
            }
        }
        return;
    }

    if (!m_Enabled) return;

    // Rising edge only (`& 1`): a single press triggers the desync. Holding
    // the key does NOT re-fire every frame.
    if ((GetAsyncKeyState(m_KeyCode) & 1) != 0)
    {
        m_Debug_LastTriggerSucceeded = SafeTriggerDesync();
        if (m_Debug_LastTriggerSucceeded)
        {
            m_Debug_DesyncWrites++;
        }
    }

    // Auto-desync on assassination state (rising edge only -- the same edge
    // idiom as the hotkey, so holding the state does NOT re-fire every frame).
    //
    // Class gate: with "only targets" enabled, the assassination is allowed
    // when the highlighted victim is a target-class NPC (SubDescriptorType in
    // m_AllowedClasses). Unknown/invalid victims fall through to the desync,
    // preserving the default behavior - the gate only ever RELAXES it.
    if (m_AutoDesyncOnAssassination && assassinationEdge)
    {
        const bool allowedClass =
            m_OnlyTargetsAllowed && victim.victimValid &&
            IsClassInAllowlist(victim.subDescriptorType, m_AllowedClasses);

        if (allowedClass)
        {
            m_Debug_AllowedAssassinations++;
        }
        else
        {
            m_Debug_AssassinationDetections++;
            m_Debug_LastTriggerSucceeded = SafeTriggerDesync();
            if (m_Debug_LastTriggerSucceeded)
            {
                m_Debug_DesyncWrites++;
            }
        }
    }
}

void DesyncKeybindPlugin::OnImGuiRender()
{
    if (ImGui::Checkbox("Enable desync hotkey", &m_Enabled))
    {
        SaveSettings();
    }

    ImGui::Text("Desync Key: 0x%02X (%d)", m_KeyCode, m_KeyCode);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey ? "Cancel" : "Rebind"))
    {
        m_WaitingForKey = !m_WaitingForKey;
    }
    if (m_WaitingForKey)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.20f, 1.00f), "Press any key... (Esc cancels)");
    }

    ImGui::Separator();

    if (ImGui::Button("Trigger Desync Now (test)"))
    {
        m_Debug_LastTriggerSucceeded = SafeTriggerDesync();
        if (m_Debug_LastTriggerSucceeded)
        {
            m_Debug_DesyncWrites++;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(isolates the write from the hotkey)");

    if (ImGui::Checkbox("Auto-desync on assassination state", &m_AutoDesyncOnAssassination))
    {
        SaveSettings();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Assassination_PP / Assassination_P / Assassination_SecondHalf_mb)");

    if (ImGui::Checkbox("Only allow assassination on target-class NPCs", &m_OnlyTargetsAllowed))
    {
        SaveSettings();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(desyncs assassinations on any other class)");
    if (ImGui::InputText("Allowed NPC classes (CSV)", m_AllowedClasses, sizeof(m_AllowedClasses)))
    {
        SaveSettings();
    }

    ImGui::Separator();
    ImGui::Text("Debug");
    ImGui::Text("GetPlayerHealth() chain:");
    ImGui::Text("  Camera component: %s", m_Debug_CameraComponentValid ? "OK" : "NULL");
    ImGui::Text("  Player entity:    %s", m_Debug_PlayerEntityValid ? "OK" : "NULL");
    ImGui::Text("  BhvAssassin:      %s", m_Debug_BhvAssassinValid ? "OK" : "NULL");
    ImGui::Text("  Health:           %s", m_Debug_HealthValid ? "OK" : "NULL");
    ImGui::Text("Assassination:     %s   Detections: %d",
        m_Debug_InAssassinationState ? "IN-STATE" : "clear",
        m_Debug_AssassinationDetections);
    ImGui::Text("Victim NPC:        %s  desc-type=%u  class=%u  prop=%u",
        m_Debug_VictimValid ? "OK" : "INVALID",
        m_Debug_VictimDescriptorType, m_Debug_VictimSubDescriptorType,
        m_Debug_VictimExplicitProperty);
    ImGui::Text("Allowed by gate:   %d", m_Debug_AllowedAssassinations);
    ImGui::Text("Desync writes: %d   Last: %s", m_Debug_DesyncWrites,
        m_Debug_LastTriggerSucceeded ? "OK" : "FAILED");
}