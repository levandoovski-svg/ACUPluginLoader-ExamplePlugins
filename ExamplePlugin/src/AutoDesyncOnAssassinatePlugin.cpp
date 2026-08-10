#include "pch.h"
#include "AutoDesyncOnAssassinatePlugin.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/CSrvPlayerHealth.h"

// Assassination human-state Enter addresses (all three sub-states count):
//   Assassination_PP
//   Assassination_P
//   Assassination_SecondHalf_mb
static constexpr uint64_t kAssassinateNodeEnter[] =
{
    0x141A4A4B0, // Assassination_PP
    0x141A462F0, // Assassination_P
    0x141A40BE0, // Assassination_SecondHalf_mb
};

// Plain-old-data only - no members with destructors/constructors that would
// require C++ object unwinding. That's required so this struct can be used
// from inside a __try block below (MSVC error C2712 otherwise).
struct ScanResultPOD
{
    bool     holderValid;
    int      receiverCount;
    bool     matched;
    uint64_t seenAddrs[40];
    int      seenCount;
};

// Raw scan logic. No local C++ objects with destructors are declared in this
// function, so it's safe to call from within __try.
static void ScanState_raw(ScanResultPOD& out)
{
    out.holderValid   = false;
    out.receiverCount = 0;
    out.matched       = false;
    out.seenCount      = 0;

    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) return;
    out.holderValid = true;

    for (auto& r : hs->primaryCallbackReceivers)
    {
        if (!r.pNode) continue;
        out.receiverCount++;

        uint64_t addr = (uint64_t)r.pNode->Enter;

        if (out.seenCount < 40)
        {
            bool already = false;
            for (int i = 0; i < out.seenCount; ++i)
                if (out.seenAddrs[i] == addr) { already = true; break; }
            if (!already)
                out.seenAddrs[out.seenCount++] = addr;
        }

        for (uint64_t enterAddr : kAssassinateNodeEnter)
            if (addr == enterAddr)
                out.matched = true;
    }
}

// SEH wrapper: primaryCallbackReceivers can hold entries for any currently-active
// human state, not just ours, and their pNode pointers can go stale between the
// engine freeing a state and this array being cleaned up. A dangling pNode causes
// a hard access violation when we read pNode->Enter. Catch that here instead of
// taking the whole game down. Everything touched inside __try is POD only.
static bool SafeScanState(ScanResultPOD& out)
{
    __try
    {
        ScanState_raw(out);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out.holderValid   = false;
        out.receiverCount = 0;
        out.matched       = false;
        out.seenCount      = 0;
        return false;
    }
}

// Also POD-only inside the __try, for the same reason.
static bool TriggerDesync_raw()
{
    CSrvPlayerHealth* health = ACU::GetPlayerHealth();
    if (!health) return false;
    health->isDesynchronizationNow = true;
    return true;
}

static void SafeTriggerDesync()
{
    __try
    {
        TriggerDesync_raw();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Pointer chain resolved to something invalid this frame - drop it,
        // don't crash the process over a missed desync.
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

static bool g_PrevAssassinateState = false;

void AutoDesyncOnAssassinatePlugin::LoadSettings() {}

void AutoDesyncOnAssassinatePlugin::OnUpdate()
{
    // Debug readout always refreshes, even if the plugin itself is disabled,
    // so you can watch the detector's raw view of the world independent of
    // whether auto-desync is toggled on.
    m_Debug_ModuleBase = (uint64_t)GetModuleHandleA(NULL);

    ScanResultPOD scan{};
    SafeScanState(scan);

    m_Debug_HolderValid      = scan.holderValid;
    m_Debug_ReceiverCount    = scan.receiverCount;
    m_Debug_MatchedThisFrame = scan.matched;

    // std::vector is only touched here, outside any __try scope.
    m_Debug_LastSeenAddrs.assign(scan.seenAddrs, scan.seenAddrs + scan.seenCount);

    ChainResultPOD chain{};
    SafeCheckChain(chain);
    m_Debug_CameraComponentValid = chain.cameraComponentValid;
    m_Debug_PlayerEntityValid    = chain.playerEntityValid;
    m_Debug_BhvAssassinValid     = chain.bhvAssassinValid;
    m_Debug_HealthValid          = chain.healthValid;

    if (!m_Enabled) return;

    bool assassinatingNow = scan.matched;
    bool risingEdge        = assassinatingNow && !g_PrevAssassinateState;
    g_PrevAssassinateState  = assassinatingNow;

    if (risingEdge)
    {
        SafeTriggerDesync();
    }
}

void AutoDesyncOnAssassinatePlugin::OnImGuiRender()
{
    ImGui::Checkbox("Auto-Desync On Assassination", &m_Enabled);

    ImGui::Separator();
    ImGui::Text("Module base: 0x%llX %s", (unsigned long long)m_Debug_ModuleBase,
        (m_Debug_ModuleBase == 0x140000000ULL) ? "(expected)" : "<-- MISMATCH, addresses are likely all wrong");

    ImGui::Separator();
    ImGui::Text("Debug");
    ImGui::Text("HumanStatesHolder valid: %s", m_Debug_HolderValid ? "yes" : "NO (player not resolved)");
    ImGui::Text("Receivers this frame: %d", m_Debug_ReceiverCount);
    ImGui::Text("Assassination match this frame: %s", m_Debug_MatchedThisFrame ? "YES" : "no");

    ImGui::Separator();
    ImGui::Text("GetPlayerHealth() chain");
    ImGui::Text("  Camera component: %s", m_Debug_CameraComponentValid ? "OK" : "NULL");
    ImGui::Text("  Player entity:    %s", m_Debug_PlayerEntityValid ? "OK" : "NULL");
    ImGui::Text("  BhvAssassin:      %s", m_Debug_BhvAssassinValid ? "OK" : "NULL");
    ImGui::Text("  Health:           %s", m_Debug_HealthValid ? "OK" : "NULL");
    ImGui::Separator();

    if (ImGui::Button("Force Desync Now (test)"))
    {
        SafeTriggerDesync();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(isolates the desync write from detection)");

    if (ImGui::TreeNode("Enter addresses seen this frame"))
    {
        if (m_Debug_LastSeenAddrs.empty())
        {
            ImGui::Text("(none)");
        }
        else
        {
            for (uint64_t addr : m_Debug_LastSeenAddrs)
            {
                bool isTarget = false;
                for (uint64_t t : kAssassinateNodeEnter) if (t == addr) isTarget = true;
                ImGui::Text("0x%llX%s", (unsigned long long)addr, isTarget ? "  <-- target" : "");
            }
        }
        ImGui::TreePop();
    }
}
