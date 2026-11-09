#include "pch.h"
#include "SwingToSwingWidenPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include <shlobj.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

// Mechanism reference, written for reuse in future parkour plugins:
// MODLOG/sidehop-angle-widen.md - the shared fit-chain gates, the field layout,
// why widening the cone changes acceptance but never aiming, and a recipe.

static SwingToSwingWidenPlugin* g_Plugin = nullptr;

// ============================================================================
//  1.5.0 target constants (dump-verified; base 0x140000000, no ASLR)
// ============================================================================

// Fancy vtable of EnumParkourAction::swing_31_swingToSwing_mb (49). Lives in the
// writable .data section; the 0x45/0x64 entries below are the version sanity
// checks that this is still the type-49 table before we repoint anything.
static constexpr uintptr_t k_SwingToSwing_FancyVTable = 0x143A77E90;
static constexpr uintptr_t k_SwingToSwing_GetEnumParkourAction = 0x1401B59F0;
static constexpr uintptr_t k_SwingToSwing_SetRanges = 0x1401B60A0;
static constexpr uintptr_t k_SharedQuickCheckAcceptableDistanceRanges = 0x1401B5890;

// Slot 0x70 of a parkour fancy vtable: QuickCheckAcceptableDistanceRanges.
// The game calls it as `mov rax,[rcx+8]; call qword ptr [rax+0xE10]`.
static constexpr int k_Slot_QuickCheck = 0x70;

using QuickCheckFn = bool(__fastcall*)(AvailableParkourAction*, EnumParkourMode);

static QuickCheckFn g_OriginalQuickCheck = nullptr;

// ============================================================================
//  Table-slot thunk: apply the widen, then run the vanilla check.
//  No game code bytes are patched anywhere, which is what keeps this plugin
//  compatible with Freeform 2.0 and with Sidehop360's Hook C cave.
// ============================================================================
static bool __fastcall WidenSwingToSwingQuickCheck(AvailableParkourAction* action, EnumParkourMode mode)
{
    SwingToSwingWidenPlugin* plugin = g_Plugin;
    if (plugin)
    {
        plugin->m_TelChecksSeen++;
        if (action && plugin->ShouldWiden())
            plugin->ApplyOverrides(*action);
    }
    if (!g_OriginalQuickCheck)
        return false;
    return g_OriginalQuickCheck(action, mode);
}

// ============================================================================
//  Slot swap (install/restore)
// ============================================================================

static FancyVFunction* SwingTable()
{
    return reinterpret_cast<FancyVFunction*>(k_SwingToSwing_FancyVTable);
}

bool SwingToSwingWidenPlugin::TryInstallSlotSwap()
{
    if (m_SlotInstalled)
        return true;

    FancyVFunction* table = SwingTable();
    if (!table)
        return false;

    // Version sanity: this must still be the type-49 table.
    if (table[0x45].fn != k_SwingToSwing_GetEnumParkourAction
        || table[0x64].fn != k_SwingToSwing_SetRanges)
    {
        m_SlotRefused = true;
        m_RefusalReason = "type-49 fancy vtable at 0x143A77E90 does not look like "
                          "the ACU 1.5.0 table (slot 0x45/0x64 mismatch) - wrong game version?";
        return false;
    }

    FancyVFunction& slot = table[k_Slot_QuickCheck];
    if (slot.fn != k_SharedQuickCheckAcceptableDistanceRanges)
    {
        m_SlotRefused = true;
        char buf[220];
        sprintf_s(buf, "slot 0x70 already repointed (currently 0x%llX, expected 0x%llX) "
                       "- another plugin owns it", (unsigned long long)slot.fn,
                  (unsigned long long)k_SharedQuickCheckAcceptableDistanceRanges);
        m_RefusalReason = buf;
        return false;
    }

    // The table is already in a writable section (.data), but keep the write
    // explicit so a future build layout cannot turn this into a crash.
    DWORD oldProtect = 0;
    if (!VirtualProtect(&slot.fn, sizeof(slot.fn), PAGE_READWRITE, &oldProtect))
        return false;

    g_OriginalQuickCheck = reinterpret_cast<QuickCheckFn>(slot.fn);
    slot.fn = reinterpret_cast<uint64>(&WidenSwingToSwingQuickCheck);

    VirtualProtect(&slot.fn, sizeof(slot.fn), oldProtect, &oldProtect);

    m_SlotInstalled = true;
    m_SlotRefused = false;
    m_RefusalReason.clear();
    m_TelSlotInstalls++;
    return true;
}

void SwingToSwingWidenPlugin::RemoveSlotSwap()
{
    if (!m_SlotInstalled)
        return;

    FancyVFunction& slot = SwingTable()[k_Slot_QuickCheck];
    // Only put the original back if the entry is still ours - if someone else
    // repointed it after us, leave their pointer alone.
    if (slot.fn == reinterpret_cast<uint64>(&WidenSwingToSwingQuickCheck))
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(&slot.fn, sizeof(slot.fn), PAGE_READWRITE, &oldProtect))
        {
            slot.fn = reinterpret_cast<uint64>(g_OriginalQuickCheck);
            VirtualProtect(&slot.fn, sizeof(slot.fn), oldProtect, &oldProtect);
        }
    }
    g_OriginalQuickCheck = nullptr;
    m_SlotInstalled = false;
}

// ============================================================================
//  Overrides - exactly the fields the shared check consumes, and only those.
//  +0x1D4/+0x1DC/+0x1E4 are ABSOLUTE upper bounds (see the header comment).
// ============================================================================
void SwingToSwingWidenPlugin::ApplyOverrides(AvailableParkourAction& action)
{
    m_TelLastMinCosAngleSeen = action.minCosAngle;

    action.minCosAngle = m_MinCosAngle;

    action.heightDifferenceMin = m_HeightDiffMin;
    action.heightDifferenceMax = m_HeightDiffMax;

    // Same generous windows the sidehop widen uses: the swing is a long move,
    // so a fixed 30/40 m ceiling replaces the vanilla 13 m ones.
    action.horizontalDifferenceMin = 0.05f;
    action.horizontalDifferenceMax = 30.0f;
    action.distanceMin = 0.05f;
    action.distanceMax = 40.0f;

    m_TelOverridesApplied++;
}

// ============================================================================
//  Lifecycle
// ============================================================================

void SwingToSwingWidenPlugin::OnBeforeActivate()
{
    g_Plugin = this;
    LoadSettings();
}

bool SwingToSwingWidenPlugin::ShouldWiden() const
{
    return m_Mode != 0
        && !m_WaitingForHotkey
        && (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
}

void SwingToSwingWidenPlugin::OnUpdate()
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

    // -- Slot swap follows the mode. While refused, retry as soon as the entry
    //    is back to the expected vanilla pointer (e.g. the other plugin that
    //    owned it got disabled), so no restart is needed. ---------------------
    if (IsSlotAllowed())
    {
        if (m_SlotRefused && SwingTable()[k_Slot_QuickCheck].fn == k_SharedQuickCheckAcceptableDistanceRanges)
        {
            m_SlotRefused = false;
            m_RefusalReason.clear();
        }
        if (!m_SlotInstalled && !m_SlotRefused)
            TryInstallSlotSwap();
    }
    else if (m_SlotInstalled)
    {
        RemoveSlotSwap();
    }
}

// ============================================================================
//  Settings
// ============================================================================

std::string SwingToSwingWidenPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\SwingToSwingWiden.ini";
    return "SwingToSwingWiden.ini";
}

void SwingToSwingWidenPlugin::LoadSettings()
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

        ParseInt  ("Mode=",            m_Mode);
        ParseInt  ("Hotkey=",          m_Hotkey);
        ParseFloat("MinCosAngle=",     m_MinCosAngle);
        ParseFloat("HeightDiffMin=",   m_HeightDiffMin);
        ParseFloat("HeightDiffMax=",   m_HeightDiffMax);
    }
}

void SwingToSwingWidenPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (!f) return;
        f << "Mode=" << m_Mode << "\n";
        f << "Hotkey=0x" << std::hex << m_Hotkey << std::dec << "\n";
        f << "MinCosAngle=" << m_MinCosAngle << "\n";
        f << "HeightDiffMin=" << m_HeightDiffMin << "\n";
        f << "HeightDiffMax=" << m_HeightDiffMax << "\n";
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

void SwingToSwingWidenPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("Swing-to-Swing Angle Widen (ACU 1.5.0)");
    ImGui::Separator();

    static const char* modes[] = { "Off", "Angle widen" };
    ImGui::SetNextItemWidth(240);
    if (ImGui::Combo("Mode##s2s", &m_Mode, modes, IM_ARRAYSIZE(modes)))
        SaveSettings();
    ImGui::TextDisabled("Widens the swing-to-swing move (type 49) only, gated by the "
                        "hold-hotkey. No game code is patched, so Freeform 2.0 and "
                        "Sidehop 360 keep working.");

    // -- Mechanism status --------------------------------------------------
    if (ImGui::CollapsingHeader("Mechanism status", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        ImGui::Text("Fancy-vtable slot 0x70 (0x143A77E90):");
        ImGui::SameLine();
        if (m_SlotInstalled)
            ImGui::TextColored(activeColor, "SWAPPED -> widen thunk");
        else if (m_SlotRefused)
            ImGui::TextColored(inactiveColor, "REFUSED");
        else
            ImGui::TextColored(inactiveColor, "not used by this mode");

        if (m_SlotRefused)
            ImGui::TextWrapped("%s", m_RefusalReason.c_str());

        ImGui::TextDisabled("The thunk applies the overrides, then calls the vanilla "
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
        ImGui::Text("Swing checks seen: %lld", m_TelChecksSeen);
        ImGui::Text("Widen overrides applied: %lld", m_TelOverridesApplied);
        ImGui::Text("Vanilla minCosAngle seen: %.4f (0.866/0.707/0.966 = cos30/45/15)",
                    m_TelLastMinCosAngleSeen);
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
            if (ImGui::SmallButton("Cancel##s2s"))
                m_WaitingForHotkey = false;
        }
        else if (ImGui::Button((std::string(KeyName(m_Hotkey)) + "##s2shotkey").c_str()))
        {
            m_WaitingForHotkey = true;
            m_CaptureArmed = false;
        }

        if (m_Mode != 0)
        {
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("minCosAngle##s2s", &m_MinCosAngle, -1.0f, 1.0f, "%.3f"))
                SaveSettings();
            ImGui::SameLine();
            if (ImGui::SmallButton("360##s2s")) { m_MinCosAngle = -1.0f; SaveSettings(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("tightest vanilla##s2s")) { m_MinCosAngle = 0.966f; SaveSettings(); }
            ImGui::TextDisabled("-1 = full 360 degrees. Vanilla per mode: ParkourUp cos30 "
                                "(0.866), Straight cos45 (0.7071), Down cos15 (0.966); "
                                "mode 0 never writes the cone.");

            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Allowed height min##s2s", &m_HeightDiffMin, -30.0f, 0.0f, "%.1f m"))
                SaveSettings();
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Allowed height max##s2s", &m_HeightDiffMax, 0.0f, 30.0f, "%.1f m"))
                SaveSettings();
            ImGui::TextDisabled("Absolute dest-height window around the src anchor "
                                "(vanilla swing: -2..+1 / -1..0 / -12..0 m depending on "
                                "the parkour mode). Horizontal and distance windows are "
                                "widened to 30/40 m automatically.");
        }

        ImGui::Unindent();
    }
}
