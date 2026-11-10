#include "pch.h"
#include "ParkourMoveWidenPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include <shlobj.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

// Mechanism reference, written for reuse in future parkour plugins:
// MODLOG/sidehop-angle-widen.md - the shared fit-chain gates, the field layout
// (including the absolute-vs-offset correction), and the table-slot recipe.

static ParkourMoveWidenPlugin* g_Plugin = nullptr;

// ============================================================================
//  1.5.0 constants (no ASLR; addresses match MODLOG/scripts/ACU_dumped.bin)
// ============================================================================

static constexpr int    k_Slot_GetEnum = 0x45;
static constexpr int    k_Slot_QuickCheck = 0x70;
static constexpr size_t k_FancyStride = 0x20;
static constexpr size_t k_FancyCount = 0x78;
static constexpr uint32_t k_ParkourTableHash0 = 0x090A5560; // nameHash of entry 0 in every parkour table
static constexpr uintptr_t k_TableScanBegin = 0x1439E0000;
static constexpr uintptr_t k_TableScanEnd = 0x143A80000;

using QuickCheckFn = bool(__fastcall*)(AvailableParkourAction*, EnumParkourMode);

// One saved original per installed slot (a move can own up to 3 tables in 1.5.0).
static constexpr int k_ThunkCount = 8;
static QuickCheckFn g_ThunkOrig[k_ThunkCount] = {};

// ============================================================================
//  Move names for the dropdown, straight from the enum's X-macro list.
// ============================================================================
#define XX(name, ignored) #name,
static const char* k_MoveNames[] = { ENUM_AS_MACRO_EnumParkourAction(XX) };
#undef XX
static const int k_MoveCount = static_cast<int>(sizeof(k_MoveNames) / sizeof(k_MoveNames[0]));

static FancyVFunction& SlotRef(uintptr_t table, int idx)
{
    return *reinterpret_cast<FancyVFunction*>(table + idx * k_FancyStride);
}

// ============================================================================
//  The thunks. One per possible installed slot so each knows its own original
//  implementation; every table of the selected move gets one.
// ============================================================================
template<int I>
static bool __fastcall WidenThunk(AvailableParkourAction* action, EnumParkourMode mode)
{
    ParkourMoveWidenPlugin* plugin = g_Plugin;
    if (plugin)
    {
        plugin->m_TelChecksSeen++;
        if (action && plugin->ShouldWiden())
            plugin->ApplyOverrides(*action);
    }
    return g_ThunkOrig[I] ? g_ThunkOrig[I](action, mode) : false;
}

static const QuickCheckFn k_Thunks[k_ThunkCount] = {
    &WidenThunk<0>, &WidenThunk<1>, &WidenThunk<2>, &WidenThunk<3>,
    &WidenThunk<4>, &WidenThunk<5>, &WidenThunk<6>, &WidenThunk<7>,
};

// ============================================================================
//  Discovery: find every parkour fancy vtable and read its action type.
// ============================================================================
void ParkourMoveWidenPlugin::DiscoverTables()
{
    m_TableCount = 0;
    const uintptr_t span = k_FancyCount * k_FancyStride;
    for (uintptr_t a = k_TableScanBegin; a + span <= k_TableScanEnd; a += 4)
    {
        if (*reinterpret_cast<const uint32_t*>(a) != k_ParkourTableHash0)
            continue;

        bool shaped = true;
        for (size_t i = 0; i < k_FancyCount; ++i)
        {
            if (*reinterpret_cast<const uint32_t*>(a + i * k_FancyStride + 4) != static_cast<uint32_t>(i))
            {
                shaped = false;
                break;
            }
        }
        if (!shaped)
            continue;

        const uintptr_t enumFn = *reinterpret_cast<const uint64_t*>(a + k_Slot_GetEnum * k_FancyStride + 0x10);
        if (!enumFn)
            continue;

        // The type is encoded in `mov eax, <enum>; ret`.
        const uint8_t* code = reinterpret_cast<const uint8_t*>(enumFn);
        int type = -1;
        if (code[0] == 0xB8 && code[5] == 0xC3)
            type = *reinterpret_cast<const int*>(code + 1);
        if (type < 0)
            continue; // table without a decodable type (not a parkour action table)

        if (m_TableCount >= k_MaxTables)
            break;

        m_TableAddr[m_TableCount] = a;
        m_TableTypeArr[m_TableCount] = type;
        m_TableCheck[m_TableCount] = *reinterpret_cast<const uint64_t*>(a + k_Slot_QuickCheck * k_FancyStride + 0x10);
        ++m_TableCount;
    }
}

int ParkourMoveWidenPlugin::TableType(int i) const
{
    return (i >= 0 && i < m_TableCount) ? m_TableTypeArr[i] : -1;
}

uintptr_t ParkourMoveWidenPlugin::TableAddress(int i) const
{
    return (i >= 0 && i < m_TableCount) ? m_TableAddr[i] : 0;
}

uintptr_t ParkourMoveWidenPlugin::TableCheckImpl(int i) const
{
    return (i >= 0 && i < m_TableCount) ? m_TableCheck[i] : 0;
}

uintptr_t ParkourMoveWidenPlugin::InstalledTable(int i) const
{
    if (i < 0 || i >= m_InstalledSlotCount)
        return 0;
    return m_TableAddr[m_InstalledTableIndex[i]];
}

bool ParkourMoveWidenPlugin::InstalledIsCustomCheck(int i) const
{
    if (i < 0 || i >= m_InstalledSlotCount)
        return false;
    return m_TableCheck[m_InstalledTableIndex[i]] != 0x1401B5890;
}

// ============================================================================
//  Slot swap for the selected move
// ============================================================================
void ParkourMoveWidenPlugin::RestoreAll()
{
    for (int s = 0; s < m_InstalledSlotCount; ++s)
    {
        const int tableIndex = m_InstalledTableIndex[s];
        FancyVFunction& slot = SlotRef(m_TableAddr[tableIndex], k_Slot_QuickCheck);
        if (slot.fn == reinterpret_cast<uint64>(k_Thunks[m_InstalledThunk[s]]))
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(&slot.fn, sizeof(slot.fn), PAGE_READWRITE, &oldProtect))
            {
                slot.fn = reinterpret_cast<uint64>(g_ThunkOrig[m_InstalledThunk[s]]);
                VirtualProtect(&slot.fn, sizeof(slot.fn), oldProtect, &oldProtect);
            }
        }
        g_ThunkOrig[m_InstalledThunk[s]] = nullptr;
    }
    m_InstalledSlotCount = 0;
    m_InstalledMove = -1;
}

bool ParkourMoveWidenPlugin::InstallForSelectedMove()
{
    RestoreAll();
    m_InstallRefused = false;
    m_InstallMessage.clear();

    int found = 0;
    int swapped = 0;
    for (int i = 0; i < m_TableCount && swapped < k_ThunkCount; ++i)
    {
        if (m_TableTypeArr[i] != m_SelectedMove)
            continue;
        ++found;

        FancyVFunction& slot = SlotRef(m_TableAddr[i], k_Slot_QuickCheck);
        if (!slot.fn)
            continue;

        DWORD oldProtect = 0;
        if (!VirtualProtect(&slot.fn, sizeof(slot.fn), PAGE_READWRITE, &oldProtect))
            continue;

        g_ThunkOrig[swapped] = reinterpret_cast<QuickCheckFn>(slot.fn);
        slot.fn = reinterpret_cast<uint64>(k_Thunks[swapped]);
        VirtualProtect(&slot.fn, sizeof(slot.fn), oldProtect, &oldProtect);

        m_InstalledTableIndex[swapped] = i;
        m_InstalledThunk[swapped] = swapped;
        ++swapped;
    }

    m_InstalledSlotCount = swapped;
    m_InstalledMove = m_SelectedMove;

    if (found == 0)
    {
        m_InstallRefused = true;
        char buf[200];
        sprintf_s(buf, "No fancy vtable with type %d was found in this image (%d tables discovered "
                       "in total) - wrong game version?", m_SelectedMove, m_TableCount);
        m_InstallMessage = buf;
        return false;
    }
    if (swapped == 0)
    {
        m_InstallRefused = true;
        m_InstallMessage = "The table(s) for this move were found but their check slot could not be "
                           "repointed (owned or unreadable).";
        return false;
    }
    return true;
}

// ============================================================================
//  Overrides
// ============================================================================
void ParkourMoveWidenPlugin::ApplyOverrides(AvailableParkourAction& action)
{
    m_TelLastMinCosAngleSeen = action.minCosAngle;

    action.minCosAngle = m_MinCosAngle;

    action.heightDifferenceMin = m_HeightDiffMin;
    action.heightDifferenceMax = m_HeightDiffMax;

    action.horizontalDifferenceMin = 0.05f;
    action.horizontalDifferenceMax = m_HorizontalMax;
    action.distanceMin = 0.05f;
    action.distanceMax = m_DistanceMax;

    m_TelOverrides++;
}

// ============================================================================
//  Lifecycle
// ============================================================================
void ParkourMoveWidenPlugin::OnBeforeActivate()
{
    g_Plugin = this;
    LoadSettings();
    DiscoverTables();
}

bool ParkourMoveWidenPlugin::ShouldWiden() const
{
    return m_Mode == 1
        && !m_WaitingForHotkey
        && (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
}

void ParkourMoveWidenPlugin::OnUpdate()
{
    // Hotkey rebind capture: arm once every key is released so the click that
    // starts the capture cannot capture itself.
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

    if (m_Mode == 0)
    {
        if (m_InstalledSlotCount > 0)
            RestoreAll();
        return;
    }

    // Selecting another move (or turning the mode back on) reinstalls: the
    // install path restores the previous swaps first.
    if (m_InstalledMove != m_SelectedMove)
        InstallForSelectedMove();
}

// ============================================================================
//  Settings
// ============================================================================
std::string ParkourMoveWidenPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\ParkourMoveWiden.ini";
    return "ParkourMoveWiden.ini";
}

void ParkourMoveWidenPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f)
    {
        SaveSettings();
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

        ParseInt  ("Mode=",          m_Mode);
        ParseInt  ("Move=",          m_SelectedMove);
        ParseInt  ("Hotkey=",        m_Hotkey);
        ParseFloat("MinCosAngle=",   m_MinCosAngle);
        ParseFloat("HeightDiffMin=", m_HeightDiffMin);
        ParseFloat("HeightDiffMax=", m_HeightDiffMax);
        ParseFloat("HorizontalMax=", m_HorizontalMax);
        ParseFloat("DistanceMax=",   m_DistanceMax);
    }
}

void ParkourMoveWidenPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (!f) return;
        f << "Mode=" << m_Mode << "\n";
        f << "Move=" << m_SelectedMove << "\n";
        f << "Hotkey=0x" << std::hex << m_Hotkey << std::dec << "\n";
        f << "MinCosAngle=" << m_MinCosAngle << "\n";
        f << "HeightDiffMin=" << m_HeightDiffMin << "\n";
        f << "HeightDiffMax=" << m_HeightDiffMax << "\n";
        f << "HorizontalMax=" << m_HorizontalMax << "\n";
        f << "DistanceMax=" << m_DistanceMax << "\n";
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

void ParkourMoveWidenPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("Parkour Move Angle Widen (ACU 1.5.0)");
    ImGui::Separator();

    static const char* modes[] = { "Off", "Angle widen" };
    ImGui::SetNextItemWidth(240);
    if (ImGui::Combo("Mode##pmw", &m_Mode, modes, IM_ARRAYSIZE(modes)))
        SaveSettings();

    ImGui::SetNextItemWidth(360);
    if (ImGui::Combo("Move##pmw", &m_SelectedMove, k_MoveNames, k_MoveCount))
        SaveSettings();
    ImGui::TextDisabled("Widens only the selected move, by repointing its own fancy-vtable check "
                        "slot - no game code is patched, so other plugins keep working.");
    if (m_SelectedMove < 0 || m_SelectedMove >= k_MoveCount)
        ImGui::TextColored(inactiveColor, "Unknown move index.");

    // -- Status ------------------------------------------------------------
    if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        ImGui::Text("Tables discovered: %d", m_TableCount);

        int tablesForMove = 0;
        uintptr_t impl = 0;
        for (int i = 0; i < m_TableCount; ++i)
        {
            if (m_TableTypeArr[i] == m_SelectedMove)
            {
                ++tablesForMove;
                if (!impl) impl = m_TableCheck[i];
            }
        }
        ImGui::Text("Tables for this move: %d (slot 0x70 impl 0x%llX)", tablesForMove,
                    (unsigned long long)impl);
        if (tablesForMove > 0 && impl != 0x1401B5890)
            ImGui::TextColored(inactiveColor, "This move uses a CUSTOM check. The widen fields are "
                                              "only verified for the generic check - try it, but a "
                                              "custom check may read different fields.");

        ImGui::Text("Swapped slots:");
        ImGui::SameLine();
        if (m_InstalledSlotCount > 0)
            ImGui::TextColored(activeColor, "%d (move %d)", m_InstalledSlotCount, m_InstalledMove);
        else
            ImGui::TextColored(inactiveColor, "none");
        if (!m_InstallMessage.empty())
            ImGui::TextColored(inactiveColor, "%s", m_InstallMessage.c_str());
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
        ImGui::Text("Checks through the widened slot: %lld", m_TelChecksSeen);
        ImGui::Text("Widen overrides applied: %lld", m_TelOverrides);
        ImGui::Text("Vanilla minCosAngle seen: %.4f", m_TelLastMinCosAngleSeen);
        if (m_TelChecksSeen == 0 && m_InstalledSlotCount > 0)
            ImGui::TextColored(inactiveColor, "The slot is swapped but the game has not evaluated "
                                              "this move yet - approach a spot where it triggers.");
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
            if (ImGui::SmallButton("Cancel##pmw"))
                m_WaitingForHotkey = false;
        }
        else if (ImGui::Button((std::string(KeyName(m_Hotkey)) + "##pmwhotkey").c_str()))
        {
            m_WaitingForHotkey = true;
            m_CaptureArmed = false;
        }

        ImGui::SetNextItemWidth(240);
        if (ImGui::SliderFloat("minCosAngle##pmw", &m_MinCosAngle, -1.0f, 1.0f, "%.3f"))
            SaveSettings();
        ImGui::SameLine();
        if (ImGui::SmallButton("360##pmw")) { m_MinCosAngle = -1.0f; SaveSettings(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("vanilla-ish##pmw")) { m_MinCosAngle = 0.7071f; SaveSettings(); }
        ImGui::TextDisabled("-1 = accept from any direction (the widen). 0.7071 = cos45, the "
                            "sidehop's vanilla cone; most other moves use cos30/cos15 already.");

        ImGui::SetNextItemWidth(240);
        if (ImGui::SliderFloat("Allowed height min##pmw", &m_HeightDiffMin, -30.0f, 0.0f, "%.1f m"))
            SaveSettings();
        ImGui::SetNextItemWidth(240);
        if (ImGui::SliderFloat("Allowed height max##pmw", &m_HeightDiffMax, 0.0f, 30.0f, "%.1f m"))
            SaveSettings();
        ImGui::SetNextItemWidth(240);
        if (ImGui::SliderFloat("Allowed horizontal max##pmw", &m_HorizontalMax, 1.0f, 60.0f, "%.1f m"))
            SaveSettings();
        ImGui::SetNextItemWidth(240);
        if (ImGui::SliderFloat("Allowed distance max##pmw", &m_DistanceMax, 1.0f, 60.0f, "%.1f m"))
            SaveSettings();
        ImGui::TextDisabled("Bounds are ABSOLUTE (the check compares value <= e + max); the mins "
                            "are fixed at 0.05 m. Vanilla values are per move - widen is most "
                            "useful on moves whose default windows are small.");

        if (ImGui::Button("Reinstall for this move##pmw"))
            InstallForSelectedMove();

        ImGui::Unindent();
    }
}
