#include "pch.h"
#include "ParkourMultiWidenPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include <shlobj.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

// Mechanism reference: MODLOG/sidehop-angle-widen.md (fit-chain gates, field
// layout including the absolute-vs-offset correction, table-slot recipe).

static ParkourMultiWidenPlugin* g_Plugin = nullptr;

// ============================================================================
//  1.5.0 constants (no ASLR; match MODLOG/scripts/ACU_dumped.bin)
// ============================================================================
static constexpr int      k_Slot_GetEnum = 0x45;
static constexpr int      k_Slot_QuickCheck = 0x70;
static constexpr size_t   k_FancyStride = 0x20;
static constexpr size_t   k_FancyCount = 0x78;
static constexpr uint32_t k_ParkourTableHash0 = 0x090A5560;
static constexpr uintptr_t k_TableScanBegin = 0x1439E0000;
static constexpr uintptr_t k_TableScanEnd = 0x143A80000;
static constexpr uintptr_t k_GenericCheck = 0x1401B5890;

using QuickCheckFn = bool(__fastcall*)(AvailableParkourAction*, EnumParkourMode);

// ============================================================================
//  Move names, straight from the enum's X-macro list.
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
//  Installed slots: one shared thunk, per-slot saved original.
// ============================================================================
struct InstalledSlot
{
    uintptr_t table;
    QuickCheckFn original;
};
static InstalledSlot g_Slots[ParkourMultiWidenPlugin::k_MaxTables] = {};
static int g_SlotCount = 0;

static bool __fastcall MultiWidenThunk(AvailableParkourAction* action, EnumParkourMode mode)
{
    if (!action)
        return false;

    const uintptr_t vtable = reinterpret_cast<uintptr_t>(action->fancyVTable);
    QuickCheckFn original = nullptr;
    for (int i = 0; i < g_SlotCount; ++i)
    {
        if (g_Slots[i].table == vtable)
        {
            original = g_Slots[i].original;
            break;
        }
    }

    ParkourMultiWidenPlugin* plugin = g_Plugin;
    if (plugin)
    {
        plugin->m_TelChecksSeen++;
        if (original && plugin->ShouldWiden())
            plugin->ApplyOverrides(*action);
        if (!original)
            plugin->m_TelMissedTableLookups++;
    }

    if (original)
        return original(action, mode);
    // Defensive fallback only: every call should arrive through one of our slots.
    return reinterpret_cast<QuickCheckFn>(k_GenericCheck)(action, mode);
}

// ============================================================================
//  Discovery
// ============================================================================
void ParkourMultiWidenPlugin::DiscoverTables()
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

        const uint8_t* code = reinterpret_cast<const uint8_t*>(enumFn);
        int type = -1;
        if (code[0] == 0xB8 && code[5] == 0xC3)
            type = *reinterpret_cast<const int*>(code + 1);
        if (type < 0)
            continue;

        if (m_TableCount >= k_MaxTables)
            break;

        m_TableAddr[m_TableCount] = a;
        m_TableTypeArr[m_TableCount] = type;
        m_TableCheck[m_TableCount] = *reinterpret_cast<const uint64_t*>(a + k_Slot_QuickCheck * k_FancyStride + 0x10);
        ++m_TableCount;
    }
}

// ============================================================================
//  Enable set
// ============================================================================
bool ParkourMultiWidenPlugin::IsMoveEnabled(int move) const
{
    return move >= 0 && move < k_MaxMoves && m_MoveEnabled[move];
}

void ParkourMultiWidenPlugin::SetMoveEnabled(int move, bool enabled)
{
    if (move >= 0 && move < k_MaxMoves)
        m_MoveEnabled[move] = enabled;
}

int ParkourMultiWidenPlugin::EnabledMoveCount() const
{
    int n = 0;
    for (int i = 0; i < k_MoveCount && i < k_MaxMoves; ++i)
        if (m_MoveEnabled[i])
            ++n;
    return n;
}

void ParkourMultiWidenPlugin::ComputeEnabledBits(uint64_t bits[2]) const
{
    bits[0] = bits[1] = 0;
    for (int i = 0; i < k_MoveCount && i < k_MaxMoves; ++i)
        if (m_MoveEnabled[i])
            bits[i >> 6] |= (1ull << (i & 63));
}

bool ParkourMultiWidenPlugin::EnabledBitsChanged() const
{
    uint64_t bits[2];
    ComputeEnabledBits(bits);
    return bits[0] != m_InstalledBits[0] || bits[1] != m_InstalledBits[1];
}

// ============================================================================
//  Slot swap for every enabled move
// ============================================================================
void ParkourMultiWidenPlugin::RestoreAll()
{
    for (int s = 0; s < g_SlotCount; ++s)
    {
        FancyVFunction& slot = SlotRef(g_Slots[s].table, k_Slot_QuickCheck);
        if (slot.fn == reinterpret_cast<uint64>(&MultiWidenThunk))
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(&slot.fn, sizeof(slot.fn), PAGE_READWRITE, &oldProtect))
            {
                slot.fn = reinterpret_cast<uint64>(g_Slots[s].original);
                VirtualProtect(&slot.fn, sizeof(slot.fn), oldProtect, &oldProtect);
            }
        }
    }
    g_SlotCount = 0;
    m_InstalledSlotCount = 0;
    m_InstalledMode = 0;
}

bool ParkourMultiWidenPlugin::InstallForEnabledMoves()
{
    RestoreAll();
    m_InstallMessage.clear();

    uint64_t bits[2];
    ComputeEnabledBits(bits);

    int wantedTables = 0;
    int swapped = 0;
    for (int i = 0; i < m_TableCount && swapped < k_MaxTables; ++i)
    {
        const int type = m_TableTypeArr[i];
        if (type < 0 || type >= k_MaxMoves || !m_MoveEnabled[type])
            continue;
        ++wantedTables;

        FancyVFunction& slot = SlotRef(m_TableAddr[i], k_Slot_QuickCheck);
        if (!slot.fn)
            continue;

        DWORD oldProtect = 0;
        if (!VirtualProtect(&slot.fn, sizeof(slot.fn), PAGE_READWRITE, &oldProtect))
            continue;

        g_Slots[swapped].table = m_TableAddr[i];
        g_Slots[swapped].original = reinterpret_cast<QuickCheckFn>(slot.fn);
        slot.fn = reinterpret_cast<uint64>(&MultiWidenThunk);
        VirtualProtect(&slot.fn, sizeof(slot.fn), oldProtect, &oldProtect);
        ++swapped;
    }
    g_SlotCount = swapped;
    m_InstalledSlotCount = swapped;
    m_InstalledBits[0] = bits[0];
    m_InstalledBits[1] = bits[1];
    m_InstalledMode = m_Mode;

    if (wantedTables == 0)
    {
        m_InstallMessage = "None of the ticked moves has a fancy vtable in this image "
                           "(wrong game version?).";
        return false;
    }
    if (swapped < wantedTables)
    {
        char buf[160];
        sprintf_s(buf, "%d of %d tables were swapped (some slots were unreadable).", swapped, wantedTables);
        m_InstallMessage = buf;
    }
    return swapped > 0;
}

// ============================================================================
//  Overrides
// ============================================================================
void ParkourMultiWidenPlugin::ApplyOverrides(AvailableParkourAction& action)
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
void ParkourMultiWidenPlugin::OnBeforeActivate()
{
    g_Plugin = this;
    LoadSettings();
    DiscoverTables();
}

bool ParkourMultiWidenPlugin::ShouldWiden() const
{
    return m_Mode == 1
        && !m_WaitingForHotkey
        && (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
}

void ParkourMultiWidenPlugin::OnUpdate()
{
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
        if (g_SlotCount > 0)
            RestoreAll();
        return;
    }

    // Reinstall when the tick set changed, or when the mode was just turned on
    // with something ticked. m_InstalledMode prevents a per-frame retry when an
    // attempt already failed (or when nothing is ticked).
    if (EnabledBitsChanged() || (m_InstalledMode != 1 && EnabledMoveCount() > 0))
        InstallForEnabledMoves();
}

// ============================================================================
//  Settings
// ============================================================================
std::string ParkourMultiWidenPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\ParkourMultiWiden.ini";
    return "ParkourMultiWiden.ini";
}

void ParkourMultiWidenPlugin::LoadSettings()
{
    std::memset(m_MoveEnabled, 0, sizeof(m_MoveEnabled));
    m_MoveEnabled[39] = true; // sidehop by default

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
        ParseInt  ("Hotkey=",        m_Hotkey);
        ParseFloat("MinCosAngle=",   m_MinCosAngle);
        ParseFloat("HeightDiffMin=", m_HeightDiffMin);
        ParseFloat("HeightDiffMax=", m_HeightDiffMax);
        ParseFloat("HorizontalMax=", m_HorizontalMax);
        ParseFloat("DistanceMax=",   m_DistanceMax);

        if (line.rfind("Moves=", 0) == 0)
        {
            std::memset(m_MoveEnabled, 0, sizeof(m_MoveEnabled));
            std::string list = line.substr(strlen("Moves="));
            std::stringstream ss(list);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                try
                {
                    const int v = std::stoi(item);
                    if (v >= 0 && v < k_MaxMoves)
                        m_MoveEnabled[v] = true;
                }
                catch (...) {}
            }
        }
    }
}

void ParkourMultiWidenPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (!f) return;
        f << "Mode=" << m_Mode << "\n";
        f << "Hotkey=0x" << std::hex << m_Hotkey << std::dec << "\n";
        f << "Moves=";
        bool first = true;
        for (int i = 0; i < k_MoveCount && i < k_MaxMoves; ++i)
        {
            if (!m_MoveEnabled[i]) continue;
            if (!first) f << ",";
            f << i;
            first = false;
        }
        f << "\n";
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

static bool ContainsCI(const char* haystack, const char* needle)
{
    if (!needle || !*needle) return true;
    const size_t n = strlen(needle);
    for (const char* p = haystack; *p; ++p)
    {
        size_t i = 0;
        while (i < n && p[i]
               && tolower(static_cast<unsigned char>(p[i])) == tolower(static_cast<unsigned char>(needle[i])))
            ++i;
        if (i == n)
            return true;
    }
    return false;
}

void ParkourMultiWidenPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("Parkour Multi-Move Angle Widen (ACU 1.5.0)");
    ImGui::Separator();

    static const char* modes[] = { "Off", "Angle widen" };
    ImGui::SetNextItemWidth(240);
    if (ImGui::Combo("Mode##pmw", &m_Mode, modes, IM_ARRAYSIZE(modes)))
        SaveSettings();
    ImGui::TextDisabled("Tick any number of moves; all ticked moves share the widen settings below "
                        "and are gated by the hold-hotkey. No game code is patched.");

    // -- Move list ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Moves", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##pmwfilter", "filter by name...", m_Filter, IM_ARRAYSIZE(m_Filter));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear filter##pmw"))
            m_Filter[0] = 0;
        ImGui::SameLine();
        ImGui::Text("enabled: %d", EnabledMoveCount());

        bool changed = false;
        ImGui::BeginChild("##pmwlist", ImVec2(0, 220), true);
        for (int i = 0; i < k_MoveCount; ++i)
        {
            if (!ContainsCI(k_MoveNames[i], m_Filter))
                continue;
            ImGui::PushID(i);
            if (ImGui::Checkbox("##on", &m_MoveEnabled[i]))
            {
                changed = true;
                SaveSettings();
            }
            ImGui::SameLine();
            ImGui::Text("%3d  %s", i, k_MoveNames[i]);
            ImGui::PopID();
        }
        ImGui::EndChild();

        if (ImGui::SmallButton("Enable all##pmw"))
        {
            for (int i = 0; i < k_MoveCount; ++i) m_MoveEnabled[i] = true;
            changed = true;
            SaveSettings();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Disable all##pmw"))
        {
            for (int i = 0; i < k_MoveCount; ++i) m_MoveEnabled[i] = false;
            changed = true;
            SaveSettings();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reinstall##pmw"))
            InstallForEnabledMoves();

        if (changed)
            InstallForEnabledMoves(); // immediate feedback while the menu is open
        ImGui::Unindent();
    }

    // -- Status -------------------------------------------------------------
    if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        ImGui::Text("Tables discovered: %d", m_TableCount);

        int tickedWithTable = 0, tickedMissing = 0, customChecks = 0;
        for (int t = 0; t < k_MoveCount && t < k_MaxMoves; ++t)
        {
            if (!m_MoveEnabled[t])
                continue;
            bool found = false;
            for (int i = 0; i < m_TableCount; ++i)
            {
                if (m_TableTypeArr[i] != t)
                    continue;
                found = true;
                if (m_TableCheck[i] != k_GenericCheck)
                    ++customChecks;
            }
            if (found) ++tickedWithTable;
            else ++tickedMissing;
        }
        ImGui::Text("Ticked moves: %d (%d with a table, %d without)",
                    EnabledMoveCount(), tickedWithTable, tickedMissing);
        ImGui::Text("Swapped slots: %d", m_InstalledSlotCount);
        if (customChecks > 0)
            ImGui::TextColored(inactiveColor, "%d ticked table(s) use a CUSTOM check implementation; "
                                              "the widen fields are only verified against the generic "
                                              "0x1401B5890, so those moves may ignore the overrides.",
                               customChecks);
        if (!m_InstallMessage.empty())
            ImGui::TextColored(inactiveColor, "%s", m_InstallMessage.c_str());
        ImGui::Unindent();
    }

    // -- Live telemetry -----------------------------------------------------
    if (ImGui::CollapsingHeader("Live telemetry", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        const bool held = ShouldWiden();
        ImGui::Text("Hotkey held:");
        ImGui::SameLine();
        ImGui::TextColored(held ? activeColor : inactiveColor, held ? "YES" : "no");
        ImGui::Text("Checks through widened slots: %lld", m_TelChecksSeen);
        ImGui::Text("Widen overrides applied: %lld", m_TelOverrides);
        ImGui::Text("Vanilla minCosAngle seen: %.4f", m_TelLastMinCosAngleSeen);
        if (m_TelMissedTableLookups > 0)
            ImGui::TextColored(inactiveColor, "Table lookups failed: %lld (should stay 0)",
                               m_TelMissedTableLookups);
        ImGui::Unindent();
    }

    // -- Settings -----------------------------------------------------------
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
        ImGui::TextDisabled("-1 = accept from any direction (the widen). Shared by every ticked move.");

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
        ImGui::TextDisabled("Bounds are ABSOLUTE. Vanilla values are per move, so the same settings "
                            "mean different amounts of widening on different moves.");

        ImGui::Unindent();
    }
}
