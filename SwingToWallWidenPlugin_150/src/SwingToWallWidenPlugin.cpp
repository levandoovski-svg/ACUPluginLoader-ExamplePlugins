#include "pch.h"
#include "SwingToWallWidenPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include <shlobj.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

// Mechanism reference, written for reuse in future parkour plugins:
// MODLOG/sidehop-angle-widen.md (the shared fit-chain gates, the field layout,
// why widening the cone changes acceptance but never aiming, and a recipe).

static SwingToWallWidenPlugin* g_Plugin = nullptr;

// ============================================================================
//  1.5.0 targets (dump-verified; base 0x140000000, no ASLR)
// ============================================================================

// Slot 0x70 of every parkour fancy vtable: QuickCheckAcceptableDistanceRanges.
// The game calls it as `mov rax,[rcx+8]; call qword ptr [rax+0xE10]`.
static constexpr int k_Slot_QuickCheck = 0x70;
static constexpr uintptr_t k_SharedQuickCheck = 0x1401B5890;

using QuickCheckFn = bool(__fastcall*)(AvailableParkourAction*, EnumParkourMode);
static QuickCheckFn g_OriginalCheck = nullptr;

struct TableTarget
{
    uintptr_t table;
    uintptr_t getEnumFn;    // slot 0x45 - version sanity check
    uintptr_t setRangesFn;  // slot 0x64 - version sanity check
    int moveId;             // EnumParkourAction value: 46 or 47
};

// EnumParkourAction::swingToWalling_front = 46 (three vtables - different
// creation contexts, all widened) and swing_2F_SwingToSidewall_includingSpindescent
// = 47 (one vtable). All four use the same shared range check.
static constexpr TableTarget k_Targets[] = {
    { 0x1439E6120, 0x1401A7B10, 0x140145C30, 46 },
    { 0x143A0B080, 0x1401A7B10, 0x140145C30, 46 },
    { 0x143A66D20, 0x1401A7B10, 0x1401A87D0, 46 },
    { 0x143A3E170, 0x1401909E0, 0x140190DC0, 47 },
};
static_assert(sizeof(k_Targets) / sizeof(k_Targets[0]) == SwingToWallWidenPlugin::k_TargetCount,
              "target table and k_TargetCount disagree");

// ============================================================================
//  Table-slot thunk: apply the widen, then run the vanilla check.
//  No game code bytes are patched anywhere, which is what keeps this plugin
//  compatible with Freeform 2.0 and with Sidehop360's Hook C cave.
// ============================================================================
static bool __fastcall WidenSwingToWallCheck(AvailableParkourAction* action, EnumParkourMode mode)
{
    SwingToWallWidenPlugin* plugin = g_Plugin;
    if (plugin)
    {
        const int moveId = plugin->IdentifyMove(action);
        if (moveId == 46)
            plugin->m_TelChecks46++;
        else if (moveId == 47)
            plugin->m_TelChecks47++;

        if (moveId != 0 && plugin->ShouldWiden())
            plugin->ApplyOverrides(*action, moveId);
    }
    if (!g_OriginalCheck)
        return false;
    return g_OriginalCheck(action, mode);
}

// ============================================================================
//  Identification / gating
// ============================================================================

int SwingToWallWidenPlugin::IdentifyMove(const AvailableParkourAction* action) const
{
    if (!action)
        return 0;
    const uintptr_t vtable = reinterpret_cast<uintptr_t>(action->fancyVTable);
    for (const TableTarget& t : k_Targets)
    {
        if (t.table == vtable)
            return t.moveId;
    }
    return 0;
}

bool SwingToWallWidenPlugin::IsMoveEnabled(int moveId) const
{
    if (moveId == 46) return m_WidenFrontWall;
    if (moveId == 47) return m_WidenSideWall;
    return false;
}

bool SwingToWallWidenPlugin::ShouldWiden() const
{
    return m_Mode != 0
        && !m_WaitingForHotkey
        && (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
}

// ============================================================================
//  Slot swap (install/restore), one entry per table
// ============================================================================

bool SwingToWallWidenPlugin::TryInstallSlot(int i)
{
    if (m_SlotInstalled[i])
        return true;
    if (i < 0 || i >= k_TargetCount)
        return false;

    const TableTarget& t = k_Targets[i];
    FancyVFunction* table = reinterpret_cast<FancyVFunction*>(t.table);

    // Version sanity: this must still be the 1.5.0 table we dumped.
    if (table[0x45].fn != t.getEnumFn
        || table[0x64].fn != t.setRangesFn
        || table[0x70].fn != k_SharedQuickCheck)
    {
        m_SlotRefused[i] = true;
        char buf[220];
        sprintf_s(buf, "table 0x%llX does not look like the ACU 1.5.0 table "
                       "(slot 0x45=0x%llX, 0x64=0x%llX, 0x70=0x%llX) - wrong game version or "
                       "another plugin already repointed a slot",
                  (unsigned long long)t.table, (unsigned long long)table[0x45].fn,
                  (unsigned long long)table[0x64].fn, (unsigned long long)table[0x70].fn);
        m_RefusalReason[i] = buf;
        return false;
    }

    FancyVFunction& slot = table[k_Slot_QuickCheck];
    DWORD oldProtect = 0;
    if (!VirtualProtect(&slot.fn, sizeof(slot.fn), PAGE_READWRITE, &oldProtect))
        return false;

    g_OriginalCheck = reinterpret_cast<QuickCheckFn>(slot.fn);
    slot.fn = reinterpret_cast<uint64>(&WidenSwingToWallCheck);

    VirtualProtect(&slot.fn, sizeof(slot.fn), oldProtect, &oldProtect);

    m_SlotInstalled[i] = true;
    m_SlotRefused[i] = false;
    m_RefusalReason[i].clear();
    m_TelSlotInstalls++;
    return true;
}

void SwingToWallWidenPlugin::RemoveSlot(int i)
{
    if (i < 0 || i >= k_TargetCount || !m_SlotInstalled[i])
        return;

    const TableTarget& t = k_Targets[i];
    FancyVFunction& slot = reinterpret_cast<FancyVFunction*>(t.table)[k_Slot_QuickCheck];
    // Only restore when the entry is still ours - if another plugin repointed it
    // after us, leave their pointer alone.
    if (slot.fn == reinterpret_cast<uint64>(&WidenSwingToWallCheck))
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(&slot.fn, sizeof(slot.fn), PAGE_READWRITE, &oldProtect))
        {
            slot.fn = reinterpret_cast<uint64>(g_OriginalCheck);
            VirtualProtect(&slot.fn, sizeof(slot.fn), oldProtect, &oldProtect);
        }
    }
    m_SlotInstalled[i] = false;
}

// ============================================================================
//  Overrides - exactly the fields the shared check consumes.
//  +0x1D4/+0x1DC/+0x1E4 are ABSOLUTE upper bounds (see MODLOG doc).
// ============================================================================
void SwingToWallWidenPlugin::ApplyOverrides(AvailableParkourAction& action, int moveId)
{
    m_TelLastMinCosAngleSeen = action.minCosAngle;

    action.minCosAngle = m_MinCosAngle;

    action.heightDifferenceMin = m_HeightDiffMin;
    action.heightDifferenceMax = m_HeightDiffMax;

    action.horizontalDifferenceMin = 0.05f;
    action.horizontalDifferenceMax = 30.0f;
    action.distanceMin = 0.05f;
    action.distanceMax = 40.0f;

    if (moveId == 46) m_TelOverrides46++;
    else if (moveId == 47) m_TelOverrides47++;
}

// ============================================================================
//  Lifecycle
// ============================================================================

void SwingToWallWidenPlugin::OnBeforeActivate()
{
    g_Plugin = this;
    LoadSettings();
}

void SwingToWallWidenPlugin::OnUpdate()
{
    // -- Hotkey rebind capture: arm once every key is seen released, so the
    //    click that started the capture cannot capture itself. ---------------
    if (m_WaitingForHotkey)
    {
        bool anyPressed = false;
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (GetAsyncKeyState(vk) & 0x8000) { anyPressed = true; break; }
        }
        if (!anyPressed)
        {
            m_CaptureArmed = true;
        }
        else if (m_CaptureArmed)
        {
            for (int vk = 1; vk <= 0xFE; ++vk)
            {
                if (GetAsyncKeyState(vk) & 0x8000)
                {
                    m_Hotkey = vk;
                    m_WaitingForHotkey = false;
                    m_CaptureArmed = false;
                    SaveSettings();
                    break;
                }
            }
        }
        return;
    }

    // -- Slots follow mode + per-move enable. While refused, retry as soon as
    //    the entry is back to the expected vanilla pointer. -------------------
    for (int i = 0; i < k_TargetCount; ++i)
    {
        const bool wanted = m_Mode != 0 && IsMoveEnabled(k_Targets[i].moveId);
        if (wanted)
        {
            if (m_SlotRefused[i]
                && reinterpret_cast<FancyVFunction*>(k_Targets[i].table)[k_Slot_QuickCheck].fn == k_SharedQuickCheck)
            {
                m_SlotRefused[i] = false;
                m_RefusalReason[i].clear();
            }
            if (!m_SlotInstalled[i] && !m_SlotRefused[i])
                TryInstallSlot(i);
        }
        else if (m_SlotInstalled[i])
        {
            RemoveSlot(i);
        }
    }
}

// ============================================================================
//  Settings
// ============================================================================

std::string SwingToWallWidenPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\SwingToWallWiden.ini";
    return "SwingToWallWiden.ini";
}

void SwingToWallWidenPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f)
    {
        SaveSettings(); // auto-create with defaults on first run
        return;
    }
    std::string line;
    while (std::getline(f, line))
    {
        auto ParseInt = [&](const char* key, int& out) {
            if (line.rfind(key, 0) == 0) { try { out = std::stoi(line.substr(strlen(key)), nullptr, 0); } catch (...) {} }
        };
        auto ParseFloat = [&](const char* key, float& out) {
            if (line.rfind(key, 0) == 0) { try { out = std::stof(line.substr(strlen(key))); } catch (...) {} }
        };

        ParseInt  ("Mode=",           m_Mode);
        ParseInt  ("Hotkey=",         m_Hotkey);
        ParseFloat("MinCosAngle=",    m_MinCosAngle);
        ParseFloat("HeightDiffMin=",  m_HeightDiffMin);
        ParseFloat("HeightDiffMax=",  m_HeightDiffMax);
        {
            int front = m_WidenFrontWall ? 1 : 0;
            ParseInt("WidenFrontWall=", front);
            m_WidenFrontWall = front != 0;
            int side = m_WidenSideWall ? 1 : 0;
            ParseInt("WidenSideWall=", side);
            m_WidenSideWall = side != 0;
        }
    }
}

void SwingToWallWidenPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (!f) return;
        f << "Mode=" << m_Mode << "\n";
        f << "Hotkey=0x" << std::hex << m_Hotkey << std::dec << "\n";
        f << "MinCosAngle=" << m_MinCosAngle << "\n";
        f << "HeightDiffMin=" << m_HeightDiffMin << "\n";
        f << "HeightDiffMax=" << m_HeightDiffMax << "\n";
        f << "WidenFrontWall=" << (m_WidenFrontWall ? 1 : 0) << "\n";
        f << "WidenSideWall=" << (m_WidenSideWall ? 1 : 0) << "\n";
    } catch (...) {}
}

// ============================================================================
//  ImGui panel
// ============================================================================

static const char* KeyName(int vk)
{
    switch (vk)
    {
    case VK_LBUTTON: return "LMB";
    case VK_RBUTTON: return "RMB";
    case VK_MBUTTON: return "MMB";
    case VK_TAB: return "Tab";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "Shift";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "Alt";
    case VK_SPACE: return "Space";
    case VK_CAPITAL: return "Caps";
    case VK_ESCAPE: return "Escape";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    default: break;
    }
    static char buf[8];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
    {
        buf[0] = (char)vk; buf[1] = 0;
        return buf;
    }
    if (vk >= VK_F1 && vk <= VK_F12)
    {
        sprintf_s(buf, "F%d", vk - VK_F1 + 1);
        return buf;
    }
    sprintf_s(buf, "0x%02X", vk);
    return buf;
}

void SwingToWallWidenPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("Swing-to-Wall Angle Widen (ACU 1.5.0)");
    ImGui::Separator();

    static const char* modes[] = { "Off", "Angle widen" };
    ImGui::SetNextItemWidth(240);
    if (ImGui::Combo("Mode##s2w", &m_Mode, modes, IM_ARRAYSIZE(modes)))
        SaveSettings();
    ImGui::TextDisabled("Widens swing-to-front-wall (46) and swing-to-sidewall (47) "
                        "only, gated by the hold-hotkey. No game code is patched, "
                        "so Freeform 2.0 and Sidehop 360 keep working.");

    // -- Per-move enable ---------------------------------------------------
    if (ImGui::CollapsingHeader("Moves", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        if (ImGui::Checkbox("Swing -> front wall (46)", &m_WidenFrontWall))
            SaveSettings();
        if (ImGui::Checkbox("Swing -> sidewall, incl. spindescent (47)", &m_WidenSideWall))
            SaveSettings();
        ImGui::TextDisabled("Each move owns its own fancy vtable(s), so switching one "
                            "off restores vanilla for that move only.");
        ImGui::Unindent();
    }

    // -- Mechanism status --------------------------------------------------
    if (ImGui::CollapsingHeader("Mechanism status", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        for (int i = 0; i < k_TargetCount; ++i)
        {
            ImGui::Text("table 0x%llX (type %d):", (unsigned long long)k_Targets[i].table,
                        k_Targets[i].moveId);
            ImGui::SameLine();
            if (m_SlotInstalled[i])
                ImGui::TextColored(activeColor, "SWAPPED");
            else if (m_SlotRefused[i])
                ImGui::TextColored(inactiveColor, "REFUSED");
            else
                ImGui::TextColored(inactiveColor, "not used");
            if (m_SlotRefused[i])
                ImGui::TextWrapped("%s", m_RefusalReason[i].c_str());
        }
        ImGui::TextDisabled("Each thunk applies the overrides, then calls the vanilla "
                            "QuickCheckAcceptableDistanceRanges (0x1401B5890) - including "
                            "Sidehop 360's Hook C cave when that plugin owns it.");
        ImGui::Unindent();
    }

    // -- Live telemetry ----------------------------------------------------
    if (ImGui::CollapsingHeader("Live telemetry", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        const bool held = ShouldWiden();
        ImGui::Text("Hotkey held:");
        ImGui::SameLine();
        ImGui::TextColored(held ? activeColor : inactiveColor, held ? "YES" : "no");
        ImGui::Text("Front wall (46): %lld checks, %lld overrides", m_TelChecks46, m_TelOverrides46);
        ImGui::Text("Sidewall   (47): %lld checks, %lld overrides", m_TelChecks47, m_TelOverrides47);
        ImGui::Text("Vanilla minCosAngle seen: %.4f", m_TelLastMinCosAngleSeen);
        ImGui::Text("Slot swaps installed: %lld", m_TelSlotInstalls);
        ImGui::Unindent();
    }

    // -- Settings ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        ImGui::Text("Hold-hotkey:");
        ImGui::SameLine(120);
        if (m_WaitingForHotkey)
        {
            ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.20f, 1.00f), "press any key...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##s2w"))
                m_WaitingForHotkey = false;
        }
        else if (ImGui::Button((std::string(KeyName(m_Hotkey)) + "##s2whotkey").c_str()))
        {
            m_WaitingForHotkey = true;
            m_CaptureArmed = false;
        }

        if (m_Mode != 0)
        {
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("minCosAngle##s2w", &m_MinCosAngle, -1.0f, 1.0f, "%.3f"))
                SaveSettings();
            ImGui::SameLine();
            if (ImGui::SmallButton("360##s2w")) { m_MinCosAngle = -1.0f; SaveSettings(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("vanilla-ish##s2w")) { m_MinCosAngle = 0.966f; SaveSettings(); }
            ImGui::TextDisabled("-1 = full 360 degrees. Vanilla: type 46 uses cos15 "
                                "(0.966) in its shared prologue and cos45/cos30/cos15 per "
                                "mode; type 47 uses cos75 (0.259).");

            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Allowed height min##s2w", &m_HeightDiffMin, -30.0f, 0.0f, "%.1f m"))
                SaveSettings();
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Allowed height max##s2w", &m_HeightDiffMax, 0.0f, 30.0f, "%.1f m"))
                SaveSettings();
            ImGui::TextDisabled("Absolute dest-height window around the src anchor. Horizontal "
                                "and distance windows are widened to 30/40 m automatically.");
        }

        ImGui::Unindent();
    }
}
