#include "pch.h"
#include "InterruptGrabPlugin.h"
#include <shlobj.h>
#include <Xinput.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#pragma comment(lib, "xinput.lib")

// Mechanism ported from D:\puppet-native-main\ac unity tab\PuppetNativeUnity.cpp
// (the Puppet Native ASI build of the same mod).

static const uintptr_t OFF_MODE    = 0x1A00157;
static const uintptr_t OFF_RESUME  = 0x1A0015E;
static const uint8_t   ORIG[7]     = { 0x0F, 0xB6, 0x9D, 0x50, 0x03, 0x00, 0x00 };

static uint8_t*  g_cave = nullptr;
static uintptr_t g_base = 0;

// ============================================================================
//  Low-level patch helpers
// ============================================================================
static void PatchBytes(uintptr_t target, const void* bytes, size_t len)
{
    DWORD old;
    VirtualProtect((LPVOID)target, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)target, bytes, len);
    VirtualProtect((LPVOID)target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)target, len);
}

static void* AllocNear(void* target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t t  = (uintptr_t)target;
    uintptr_t lo = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t hi = (uintptr_t)si.lpMaximumApplicationAddress;
    uintptr_t start = t & ~0xFFFFull;
    for (uintptr_t a = start; a > lo && a > t - 0x70000000ull; a -= 0x10000)
    {
        void* m = VirtualAlloc((LPVOID)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (m) return m;
    }
    for (uintptr_t a = start + 0x10000; a < hi && a < t + 0x70000000ull; a += 0x10000)
    {
        void* m = VirtualAlloc((LPVOID)a, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (m) return m;
    }
    return nullptr;
}

static void WriteJmpRel32(uint8_t* out, uintptr_t from, uintptr_t to)
{
    out[0] = 0xE9;
    int32_t rel = (int32_t)((int64_t)to - (int64_t)(from + 5));
    memcpy(out + 1, &rel, 4);
}

bool InterruptGrabPlugin::BuildCave()
{
    uint8_t* site = (uint8_t*)(g_base + OFF_MODE);
    if (memcmp(site, ORIG, sizeof(ORIG)) != 0)
        return false;   // different build: do nothing

    g_cave = (uint8_t*)AllocNear(site, 0x1000);
    if (!g_cave)
        return false;

    uint8_t* p = g_cave;
    *p++ = 0x41; *p++ = 0xBE; *p++ = 0x03; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;  // mov r14d,3
    memcpy(p, ORIG, sizeof(ORIG)); p += sizeof(ORIG);                                // stolen bytes
    WriteJmpRel32(p, (uintptr_t)p, g_base + OFF_RESUME);                             // back into the driver
    FlushInstructionCache(GetCurrentProcess(), g_cave, (SIZE_T)(p + 5 - g_cave));
    return true;
}

bool InterruptGrabPlugin::ApplyPatch()
{
    if (m_PatchApplied || !g_cave)
        return m_PatchApplied;
    // shortcut: 7 bytes are written in one go, so a game thread already inside
    // those 7 bytes can see a torn jump. Patches only flip on key transitions;
    // upgrade to a hook (MinHook/CCodeInTheMiddle) if this ever crashes.
    uint8_t jmp[7];
    WriteJmpRel32(jmp, g_base + OFF_MODE, (uintptr_t)g_cave);
    jmp[5] = 0x90;
    jmp[6] = 0x90;
    PatchBytes(g_base + OFF_MODE, jmp, sizeof(jmp));
    m_PatchApplied = true;
    ++m_TelActivations;
    return true;
}

void InterruptGrabPlugin::RevertPatch()
{
    if (!m_PatchApplied)
        return;
    PatchBytes(g_base + OFF_MODE, ORIG, sizeof(ORIG));
    m_PatchApplied = false;
}

// ============================================================================
//  Pad button binding (single button, no combos - the ASI had combos, the
//  plugin deliberately does not)
// ============================================================================
struct PadToken
{
    const char* name;
    WORD buttons;    // XINPUT_GAMEPAD_*
    int  isTrigger;  // 0 = button, 1 = left trigger, 2 = right trigger
};
static const PadToken k_PadTokens[] = {
    { "NONE",  0, -1 },
    { "A",     XINPUT_GAMEPAD_A, 0 },
    { "B",     XINPUT_GAMEPAD_B, 0 },
    { "X",     XINPUT_GAMEPAD_X, 0 },
    { "Y",     XINPUT_GAMEPAD_Y, 0 },
    { "LB",    XINPUT_GAMEPAD_LEFT_SHOULDER, 0 },
    { "RB",    XINPUT_GAMEPAD_RIGHT_SHOULDER, 0 },
    { "LS",    XINPUT_GAMEPAD_LEFT_THUMB, 0 },
    { "RS",    XINPUT_GAMEPAD_RIGHT_THUMB, 0 },
    { "START", XINPUT_GAMEPAD_START, 0 },
    { "BACK",  XINPUT_GAMEPAD_BACK, 0 },
    { "UP",    XINPUT_GAMEPAD_DPAD_UP, 0 },
    { "DOWN",  XINPUT_GAMEPAD_DPAD_DOWN, 0 },
    { "LEFT",  XINPUT_GAMEPAD_DPAD_LEFT, 0 },
    { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT, 0 },
    { "LT",    0, 1 },
    { "RT",    0, 2 },
};
static constexpr int k_PadTokenCount = (int)(sizeof(k_PadTokens) / sizeof(k_PadTokens[0]));

static int PadIndexFromName(const char* s)
{
    for (int i = 0; i < k_PadTokenCount; ++i)
        if (_stricmp(s, k_PadTokens[i].name) == 0)
            return i;
    return 0;
}

// Encoded binding: token index plus trigger role. Buttons are stored as the
// XINPUT_GAMEPAD_* mask; triggers are stored as negative sentinel values.
static constexpr int k_PadTriggerLeft  = -1;
static constexpr int k_PadTriggerRight = -2;

static int EncodePadToken(int tokenIndex)
{
    if (tokenIndex <= 0) return 0;
    if (k_PadTokens[tokenIndex].isTrigger == 1) return k_PadTriggerLeft;
    if (k_PadTokens[tokenIndex].isTrigger == 2) return k_PadTriggerRight;
    return (int)k_PadTokens[tokenIndex].buttons;
}

static bool IsPadBindingHeld(const XINPUT_GAMEPAD& pad, int binding)
{
    if (binding == 0) return false;
    if (binding == k_PadTriggerLeft)  return pad.bLeftTrigger > 30;
    if (binding == k_PadTriggerRight) return pad.bRightTrigger > 30;
    return (pad.wButtons & (WORD)binding) != 0;
}

// ============================================================================
//  Lifecycle
// ============================================================================
void InterruptGrabPlugin::OnBeforeActivate()
{
    LoadSettings();

    HMODULE module = GetModuleHandleA(NULL);   // the ACU.exe image itself
    g_base = (uintptr_t)module;
    if (!g_base)
    {
        m_SupportedBuild = false;
        m_StatusMessage = "Main module not found.";
        return;
    }
    if (!BuildCave())
    {
        m_SupportedBuild = false;
        m_StatusMessage = "Site signature mismatch (wrong game version - expected ACU 1.5.0). Refusing to patch.";
        return;
    }
    m_SupportedBuild = true;
    m_StatusMessage = "Ready. Hold the hotkey while grabbing/hanging to drop off.";
}

bool InterruptGrabPlugin::ShouldPatch() const
{
    if (!m_Enabled || m_WaitingForHotkey || !m_SupportedBuild)
        return false;
    if ((GetAsyncKeyState(m_Hotkey) & 0x8000) != 0)
        return true;
    if (m_PadButton != 0)
    {
        for (DWORD slot = 0; slot < 4; ++slot)
        {
            XINPUT_STATE state;
            ZeroMemory(&state, sizeof(state));
            if (XInputGetState(slot, &state) == ERROR_SUCCESS
                && IsPadBindingHeld(state.Gamepad, m_PadButton))
                return true;
        }
    }
    return false;
}

void InterruptGrabPlugin::OnUpdate()
{
    // Keep the patch in sync with the desired state even while a rebind wait is
    // open (a wait must not leave the patch installed).
    const bool want = ShouldPatch();
    if (want && !m_PatchApplied)
        ApplyPatch();
    else if (!want && m_PatchApplied)
        RevertPatch();

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
}

// ============================================================================
//  Settings
// ============================================================================
std::string InterruptGrabPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\InterruptGrab.ini";
    return "InterruptGrab.ini";
}

void InterruptGrabPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f)
    {
        SaveSettings();   // first run: write the defaults
        return;
    }
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto ParseInt = [&](const char* key, int& out) {
            if (line.rfind(key, 0) == 0)
            {
                try { out = std::stoi(line.substr(strlen(key)), nullptr, 0); } catch (...) {}
            }
        };
        int enabled = m_Enabled ? 1 : 0;
        ParseInt("Enabled=", enabled);
        m_Enabled = enabled != 0;
        ParseInt("Hotkey=", m_Hotkey);
        if (line.rfind("Pad=", 0) == 0)
            m_PadButton = EncodePadToken(PadIndexFromName(line.substr(strlen("Pad=")).c_str()));
    }
}

void InterruptGrabPlugin::SaveSettings()
{
    try
    {
        std::ofstream f(GetIniPath(), std::ios::trunc);
        if (!f) return;
        // Decode the stored binding back to a token for a readable ini.
        const char* padName = "NONE";
        if (m_PadButton != 0)
        {
            for (int i = 1; i < k_PadTokenCount; ++i)
            {
                if (EncodePadToken(i) == m_PadButton) { padName = k_PadTokens[i].name; break; }
            }
        }
        f << "; Interrupt Grab - hold the hotkey to force a ParkourDown and drop off a ledge/grab.\n";
        f << "Enabled=" << (m_Enabled ? 1 : 0) << "\n";
        f << "Hotkey=0x" << std::hex << m_Hotkey << std::dec << "\n";
        f << "Pad=" << padName << "\n";
    }
    catch (...) {}
}

// ============================================================================
//  ImGui panel
// ============================================================================
static const char* KeyName(int vk)
{
    static char buf[8];
    switch (vk)
    {
    case VK_LBUTTON: return "LMB";
    case VK_RBUTTON: return "RMB";
    case VK_MBUTTON: return "MMB";
    case VK_SHIFT:   return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU:    return "Alt";
    case VK_SPACE:   return "Space";
    case VK_TAB:     return "Tab";
    case VK_ESCAPE:  return "Escape";
    case VK_RETURN:  return "Enter";
    case VK_BACK:    return "Backspace";
    default: break;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
    {
        buf[0] = (char)vk;
        buf[1] = 0;
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

void InterruptGrabPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("Interrupt Grab (ACU 1.5.0)");
    ImGui::Separator();

    if (!m_SupportedBuild)
    {
        ImGui::TextColored(inactiveColor, "UNSUPPORTED BUILD");
        ImGui::TextWrapped("%s", m_StatusMessage.c_str());
        return;
    }

    if (ImGui::Checkbox("Enable Interrupt Grab", &m_Enabled))
        SaveSettings();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While held, the parkour action-selection driver is forced to pick Parkour Down (3).");

    ImGui::Text("Hold the hotkey while grabbing/hanging to force a drop. Keyboard + one gamepad button.");

    // -- Keyboard hotkey ---------------------------------------------------
    ImGui::Text("Keyboard:");
    ImGui::SameLine(120);
    if (m_WaitingForHotkey)
    {
        ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.20f, 1.00f), "press any key...");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##ig"))
            m_WaitingForHotkey = false;
    }
    else if (ImGui::Button((std::string(KeyName(m_Hotkey)) + "##igkb").c_str()))
    {
        m_WaitingForHotkey = true;
        m_CaptureArmed = false;
    }

    // -- Gamepad button ----------------------------------------------------
    ImGui::Text("Gamepad:");
    ImGui::SameLine(120);
    {
        const char* items[k_PadTokenCount];
        int current = 0;
        for (int i = 0; i < k_PadTokenCount; ++i)
        {
            items[i] = k_PadTokens[i].name;
            if (EncodePadToken(i) == m_PadButton)
                current = i;
        }
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo("##igpad", &current, items, k_PadTokenCount))
        {
            m_PadButton = EncodePadToken(current);
            SaveSettings();
        }
    }

    // -- Live state --------------------------------------------------------
    const bool held = ShouldPatch();
    ImGui::Text("Hotkey held:");
    ImGui::SameLine();
    ImGui::TextColored(held ? activeColor : inactiveColor, held ? "YES" : "no");
    ImGui::Text("Patch installed:");
    ImGui::SameLine();
    ImGui::TextColored(m_PatchApplied ? activeColor : inactiveColor, m_PatchApplied ? "yes" : "no");
    ImGui::Text("Times activated: %lld", m_TelActivations);
    ImGui::TextDisabled("INI: Documents\\Assassin's Creed Unity\\InterruptGrab.ini");
}
