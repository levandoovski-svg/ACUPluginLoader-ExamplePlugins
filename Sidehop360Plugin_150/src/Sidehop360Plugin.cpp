#include "pch.h"
#include "Sidehop360Plugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ACU/Memory/ACUAllocs.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"
#include <shlobj.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

// GetEnumParkourAction() is declared by AvailableParkourAction.h but never
// called here, so (not compiling GenericHooksInParkourFiltering.cpp) it needs
// no definition. See SidehopContextPlugin.cpp for the inline-definition
// pattern should this plugin ever need to call it.

static Sidehop360Plugin* g_Plugin = nullptr;

// ============================================================================
//  Hook A - widen the sidehop acceptance cone (minCosAngle, +0x1E8)
// ============================================================================

// 0x140148500 is the sidehop's SetExpectedDistanceRangesAccordingToParkourMode
// impl (fancy vtable 0x143A13130 slot 0x64, "descent from wall to ground; also
// sidehop"). Its prologue writes the type's constants, including at
// 0x140148549 the cone `mov dword ptr [rcx+0x1e8], 0x3f3504f3` (cos 45).
// The hook sits on the NEXT instruction (0x140148553, a clean 10-byte
// `mov [rcx+0x1ec], 0x3f800000`, re-executed after the callback), so the
// callback sees minCosAngle exactly as vanilla wrote it and can override it
// before InitialTestIfActionFits (slot 0x4B = 0x1401b4900, which calls slot
// 0x64 first) or anything else consumes it. rcx is the action (`mov rdx,rcx`
// at the impl's entry).
void WidenSidehopAngle_Cave(AllRegisters* params)
{
    Sidehop360Plugin* plugin = g_Plugin;
    if (!plugin || !plugin->m_Enabled || !plugin->ShouldWidenAngle())
        return;
    auto* action = reinterpret_cast<AvailableParkourAction*>(params->rcx_);
    if (!action)
        return;
    plugin->m_TelLastMinCosAngleSeen = action->minCosAngle;
    action->minCosAngle = plugin->m_MinCosAngle;

    // Test-1: the +-45 limit lives at candidate CREATION, not in the fit cone,
    // and the sidehop expects a 2-3 m hop (set-ranges prologue writes
    // +0x238=2.0, +0x23C=3.0). Loosen the range so farther hops pass.
    action->Side_min = plugin->m_ExpectedSideMin;
    action->Side_max = plugin->m_ExpectedSideMax;

    // Cache the wall's outward normal (XY, normalized) for redirect mode 2 and
    // Hook B nudge mode 2. directionDestFacingOut (+0x40) points out of the
    // wall toward the player's face.
    {
        const float nx = action->directionDestFacingOut.x;
        const float ny = action->directionDestFacingOut.y;
        const float len = sqrtf(nx * nx + ny * ny);
        if (len > 1e-6f)
        {
            plugin->m_WallNormalX = nx / len;
            plugin->m_WallNormalY = ny / len;
            plugin->m_HasWallNormal = true;
        }
    }

    // Redirect: re-aim the candidate's target in the ground plane. Follows the
    // proven MutateIntoSidehop pattern: only dir80 + anchors + distances matter
    // (it never touches the SrcToDest vectors); keep dest.z/w from src.
    if (plugin->m_RedirectMode > 0)
    {
        float destX, destY;
        if (plugin->m_RedirectMode == 1)
        {
            // Mirror 180 degrees through the src anchor.
            destX = 2.0f * action->locationAnchorSrc.x - action->locationAnchorDest.x;
            destY = 2.0f * action->locationAnchorSrc.y - action->locationAnchorDest.y;
            action->dir80.x = -action->dir80.x;
            action->dir80.y = -action->dir80.y;
            action->dir80.z = 0.0f;
            action->dir80.w = 0.0f;
        }
        else
        {
            // Off the wall: place the target along the wall's outward normal
            // (toward the roof across the alley), HopDistance meters out.
            destX = action->locationAnchorSrc.x + plugin->m_WallNormalX * plugin->m_HopDistance;
            destY = action->locationAnchorSrc.y + plugin->m_WallNormalY * plugin->m_HopDistance;
            action->dir80.x = plugin->m_WallNormalX;
            action->dir80.y = plugin->m_WallNormalY;
            action->dir80.z = 0.0f;
            action->dir80.w = 0.0f;
        }
        action->locationAnchorDest.x = destX;
        action->locationAnchorDest.y = destY;
        action->locationAnchorDest.z = action->locationAnchorSrc.z;
        action->locationAnchorDest.w = action->locationAnchorSrc.w;
        const float dx = destX - action->locationAnchorSrc.x;
        const float dy = destY - action->locationAnchorSrc.y;
        const float horizontalLen = sqrtf(dx * dx + dy * dy);
        action->horizontalDifference = horizontalLen;
        action->distance = horizontalLen;
        plugin->m_TelRedirects++;
    }

    plugin->m_TelWidenApplications++;
}

struct SidehopAngleWidenHook : AutoAssemblerCodeHolder_Base
{
    SidehopAngleWidenHook()
    {
        PresetScript_CCodeInTheMiddle(
            0x140148553, 10,
            WidenSidehopAngle_Cave, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    }
};

AutoAssembleWrapper<SidehopAngleWidenHook>& GetAngleWidenHook()
{
    static AutoAssembleWrapper<SidehopAngleWidenHook> s_hook;
    return s_hook;
}

// ============================================================================
//  Hook B - breakfall-style position nudge (tunable extra, default off)
// ============================================================================

class ParkourAction_Commonbase;

// 1.5.0 address (ACUFixes-0.9.3 set), dump-verified function prologue.
// NOTE: FreeJumpPlugin_151's WallToHang hook still carries the 1.5.1 value
// (0x1401D1260) for this function - do not copy it from there.
DEFINE_GAME_FUNCTION(CreateParkourActionForParkourPointIfFits, 0x1401D1CF0, char, __fastcall,
    (EnumParkourAction a1, uint64 a2, __m128* a3, __m128* p_movementVecWorld_mb, float a5,
     int a6, char a7, uint64 a8, uint64 a9, uint64 p_currentLedge_mb,
     ParkourAction_Commonbase** p_newAction_out, float a12, float p_epsilon_mb));

// 0x140135230 is inside the climbFacade scan, at the point where it calls
// CreateParkourActionForParkourPointIfFits (rdx = origin-context, rdi = the
// probe point, r9 = movement vector). The 12 stolen bytes contain that call
// and are re-executed after the callback - so the vanilla sidehop is still
// created from the untouched point, and the callback adds ONE extra sidehop
// created from a nudged COPY of the probe point (breakfall-hack pattern:
// pretend the player is somewhere else so a target fits). Argument reads are
// the ones proven for this exact site by the WallToHang port.
void WhenScanClimbFacade_AlsoNudgedSidehop(AllRegisters* params)
{
    Sidehop360Plugin* plugin = g_Plugin;
    if (!plugin || !plugin->m_Enabled || !plugin->ShouldNudge())
        return;

    const __m128 originalPoint = *reinterpret_cast<__m128*>(params->rdi_);
    const __m128 movementVec = *reinterpret_cast<__m128*>(params->r9_);
    const float* mv = reinterpret_cast<const float*>(&movementVec);
    // Horizontal plane in ACU is x/y (z is up).
    // Direction: 0 = opposite of the movement input, 1 = along it,
    // 2 = off the wall (along the normal cached by Hook A).
    float dirX = mv[0], dirY = mv[1];
    float sign = plugin->m_NudgeDirectionMode == 0 ? -1.0f : 1.0f;
    if (plugin->m_NudgeDirectionMode == 2)
    {
        if (!plugin->m_HasWallNormal)
            return;
        dirX = plugin->m_WallNormalX;
        dirY = plugin->m_WallNormalY;
        sign = 1.0f;
    }
    const float len = sqrtf(dirX * dirX + dirY * dirY);
    if (len < 1e-6f)
        return;
    dirX /= len;
    dirY /= len;
    __m128 nudgedPoint = originalPoint;
    float* np = reinterpret_cast<float*>(&nudgedPoint);
    np[0] += dirX * sign * plugin->m_NudgeDistance;
    np[1] += dirY * sign * plugin->m_NudgeDistance;

    plugin->m_TelNudgeCalls++;
    bool isCreated = CreateParkourActionForParkourPointIfFits(
        EnumParkourAction::climbFacade_fromWallDescentSidewaysToGround_alsoSidehop
        , params->rdx_
        , &nudgedPoint
        , reinterpret_cast<__m128*>(params->r9_)
        , params->XMM6.f0
        , (int&)params->r13_
        , *(char*)(params->GetRSP() + 0x100)
        , *(uint64*)(*(uint64*)params->r12_ + (int&)params->r15_)
        , *(uint64*)(params->rbp_ + 0x118)
        , *(uint64*)(params->rbp_ + 0x120)
        , (ParkourAction_Commonbase**)(params->GetRSP() + 0x108)
        , *(float*)(params->rbp_ + 0x104)
        , *(float*)(params->rbp_ + 0x108));
    if (isCreated)
    {
        plugin->m_TelNudgeCreated++;
        ACU::Memory::SmallArrayAppend(*(SmallArray<ParkourAction_Commonbase*>*)params->rbx_,
            *(ParkourAction_Commonbase**)(params->GetRSP() + 0x108));
    }
}

struct SidehopNudgeHook : AutoAssemblerCodeHolder_Base
{
    SidehopNudgeHook()
    {
        PresetScript_CCodeInTheMiddle(
            0x140135230, 12,
            WhenScanClimbFacade_AlsoNudgedSidehop, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    }
};

AutoAssembleWrapper<SidehopNudgeHook>& GetNudgeHook()
{
    static AutoAssembleWrapper<SidehopNudgeHook> s_hook;
    return s_hook;
}

// The only reliable install-time signature for "someone else owns this site":
// the first bytes are their jmp cave (see ForcedSidehopPlugin::ActivateHook).
bool IsSiteTaken(uintptr_t site)
{
    const uint16_t firstBytes = *reinterpret_cast<const uint16_t*>(site);
    return (firstBytes & 0x00FF) == 0x00E9     // jmp rel32
        || firstBytes == 0x25FF                // jmp [rip+disp32]
        || firstBytes == 0xB848;               // movabs rax, imm64 (patch prologue)
}

// ============================================================================
//  Lifecycle
// ============================================================================

void Sidehop360Plugin::OnBeforeActivate()
{
    g_Plugin = this;
    LoadSettings();
}

bool Sidehop360Plugin::ShouldWidenAngle() const
{
    return !m_WaitingForHotkey
        && (GetAsyncKeyState(m_Hotkey) & 0x8000) != 0;
}

bool Sidehop360Plugin::ShouldNudge() const
{
    return ShouldWidenAngle();
}

void Sidehop360Plugin::OnUpdate()
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

    if (!m_Enabled)
    {
        if (m_HooksInstalled)
        {
            GetAngleWidenHook().Deactivate();
            GetNudgeHook().Deactivate();
            m_HooksInstalled = false;
            m_HookAInstalled = false;
            m_HookBInstalled = false;
        }
        return;
    }

    if (!m_HookAInstalled && !m_HookARefused)
    {
        if (IsSiteTaken(0x140148553))
        {
            m_HookARefused = true;
        }
        else
        {
            GetAngleWidenHook().Activate();
            m_HookAInstalled = true;
        }
    }

    // Hook B follows its own toggle and refuses on its own when ACUFixes'
    // "Parkour helpers" patch owns the site (it hooks 0x140135230 itself, in
    // Request_Spindescent.cpp). When that patch is toggled off in ACUFixes'
    // menu, the site frees up - so keep retrying while the nudge is enabled.
    if (m_NudgeEnabled)
    {
        if (!m_HookBInstalled && !m_HookBRefused)
        {
            if (IsSiteTaken(0x140135230))
                m_HookBRefused = true;
            else
            {
                GetNudgeHook().Activate();
                m_HookBInstalled = true;
            }
        }
        else if (m_HookBRefused && !IsSiteTaken(0x140135230))
        {
            m_HookBRefused = false; // site freed - install on the next pass
        }
    }
    else if (m_HookBInstalled)
    {
        GetNudgeHook().Deactivate();
        m_HookBInstalled = false;
    }
    m_HooksInstalled = m_HookAInstalled || m_HookBInstalled;
}

// ============================================================================
//  Settings
// ============================================================================

std::string Sidehop360Plugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\Sidehop360.ini";
    return "Sidehop360.ini";
}

void Sidehop360Plugin::LoadSettings()
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
        auto ParseBool = [&](const char* key, bool& out) {
            if (line.rfind(key, 0) == 0) out = line.substr(strlen(key)) == "1";
        };

        ParseBool ("Enabled=",          m_Enabled);
        ParseInt  ("Hotkey=",           m_Hotkey);
        ParseFloat("MinCosAngle=",      m_MinCosAngle);
        ParseInt  ("RedirectMode=",     m_RedirectMode);
        ParseFloat("HopDistance=",      m_HopDistance);
        ParseFloat("ExpectedSideMin=",  m_ExpectedSideMin);
        ParseFloat("ExpectedSideMax=",  m_ExpectedSideMax);
        ParseBool ("NudgeEnabled=",     m_NudgeEnabled);
        ParseInt  ("NudgeDirection=",   m_NudgeDirectionMode);
        ParseFloat("NudgeDistance=",    m_NudgeDistance);
    }
}

void Sidehop360Plugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (!f) return;
        f << "Enabled=" << (m_Enabled ? 1 : 0) << "\n";
        f << "Hotkey=0x" << std::hex << m_Hotkey << std::dec << "\n";
        f << "MinCosAngle=" << m_MinCosAngle << "\n";
        f << "RedirectMode=" << m_RedirectMode << "\n";
        f << "HopDistance=" << m_HopDistance << "\n";
        f << "ExpectedSideMin=" << m_ExpectedSideMin << "\n";
        f << "ExpectedSideMax=" << m_ExpectedSideMax << "\n";
        f << "NudgeEnabled=" << (m_NudgeEnabled ? 1 : 0) << "\n";
        f << "NudgeDirection=" << m_NudgeDirectionMode << "\n";
        f << "NudgeDistance=" << m_NudgeDistance << "\n";
    } catch (...) {}
}

// ============================================================================
//  ImGui debug panel
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

void Sidehop360Plugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("Sidehop 360 (ACU 1.5.0)");
    ImGui::Separator();
    ImGui::TextWrapped("While wall-climbing, hold the hotkey to let the sidehop target "
                       "any angle (vanilla: only +-45 degrees to your sides). "
                       "The nudge is an extra, breakfall-style mechanism.");

    bool enabled = m_Enabled;
    if (ImGui::Checkbox("Enable Sidehop 360", &enabled))
    {
        m_Enabled = enabled;
        SaveSettings();
    }

    // -- Hook status -------------------------------------------------------
    if (ImGui::CollapsingHeader("Hook status", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        ImGui::Text("Hook A - angle widen (0x140148553):");
        ImGui::SameLine();
        if (m_HookARefused)
            ImGui::TextColored(inactiveColor, "REFUSED - site patched by another plugin");
        else if (m_HooksInstalled && m_Enabled)
            ImGui::TextColored(activeColor, "INSTALLED");
        else
            ImGui::TextColored(inactiveColor, "not installed");
        ImGui::Text("Hook B - position nudge (0x140135230):");
        ImGui::SameLine();
        if (m_HookBRefused)
            ImGui::TextColored(inactiveColor,
                "REFUSED - site owned by ACUFixes' \"Parkour helpers\" patch "
                "(not ACU_ParkourPatches - corrected in v0.2.1). To test the "
                "nudge: turn Parkour helpers OFF in ACUFixes' menu, then this "
                "hook installs on its own. Do not re-enable Parkour helpers "
                "while the nudge is on.");
        else if (m_NudgeEnabled && m_HooksInstalled && m_Enabled)
            ImGui::TextColored(activeColor, "INSTALLED");
        else
            ImGui::TextColored(inactiveColor, "off");
        ImGui::Unindent();
    }

    // -- Live telemetry ----------------------------------------------------
    if (ImGui::CollapsingHeader("Live telemetry", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        const bool held = ShouldWidenAngle();
        ImGui::Text("Hotkey held:");
        ImGui::SameLine();
        ImGui::TextColored(held ? activeColor : inactiveColor, held ? "YES" : "no");
        ImGui::Text("Angle overrides applied: %lld", m_TelWidenApplications);
        ImGui::Text("Vanilla minCosAngle seen: %.4f (cos45 = 0.7071)", m_TelLastMinCosAngleSeen);
        ImGui::Text("Redirects applied: %lld (mode %d)", m_TelRedirects, m_RedirectMode);
        ImGui::Text("Nudge: %lld calls, %lld sidehops created", m_TelNudgeCalls, m_TelNudgeCreated);
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
            if (ImGui::SmallButton("Cancel##s360"))
                m_WaitingForHotkey = false;
        }
        else if (ImGui::Button((std::string(KeyName(m_Hotkey)) + "##s360hotkey").c_str()))
        {
            m_WaitingForHotkey = true;
            m_CaptureArmed = false;
        }

        ImGui::SetNextItemWidth(240);
        if (ImGui::SliderFloat("minCosAngle##s360", &m_MinCosAngle, -1.0f, 1.0f, "%.3f"))
            SaveSettings();
        ImGui::SameLine();
        if (ImGui::SmallButton("360##s360")) { m_MinCosAngle = -1.0f; SaveSettings(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("vanilla##s360")) { m_MinCosAngle = 0.7071f; SaveSettings(); }
        ImGui::TextDisabled("-1 = full 360 degrees. Vanilla writes cos45 (0.7071).");

        ImGui::Separator();
        ImGui::Text("Redirect & reach (test-1: candidates exist, but are aimed/near)");
        const char* redirectModes[] = { "Off (widen only)", "Mirror 180 degrees", "Off the wall" };
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("Redirect##s360", &m_RedirectMode, redirectModes, IM_ARRAYSIZE(redirectModes)))
            SaveSettings();
        ImGui::SetNextItemWidth(240);
        if (ImGui::SliderFloat("Hop distance##s360", &m_HopDistance, 0.5f, 10.0f, "%.2f m"))
            SaveSettings();
        ImGui::SetNextItemWidth(240);
        float sideMin = m_ExpectedSideMin;
        if (ImGui::SliderFloat("Expected hop min##s360", &sideMin, 0.0f, 5.0f, "%.2f m"))
        {
            m_ExpectedSideMin = sideMin;
            SaveSettings();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("vanilla##s360min")) { m_ExpectedSideMin = 2.0f; SaveSettings(); }
        ImGui::SetNextItemWidth(240);
        float sideMax = m_ExpectedSideMax;
        if (ImGui::SliderFloat("Expected hop max##s360", &sideMax, 1.0f, 15.0f, "%.2f m"))
        {
            m_ExpectedSideMax = sideMax;
            SaveSettings();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("vanilla##s360max")) { m_ExpectedSideMax = 3.0f; SaveSettings(); }
        ImGui::TextDisabled("Side_min/Max: hop length the sidehop accepts (vanilla 2-3 m). "
                            "Raise the max to reach farther roofs. Mirror flips the "
                            "candidate's target; Off-the-wall places it Hop distance "
                            "out along the wall normal.");

        ImGui::Separator();
        bool nudge = m_NudgeEnabled;
        if (ImGui::Checkbox("Enable position nudge (extra)", &nudge))
        {
            m_NudgeEnabled = nudge;
            SaveSettings();
        }
        if (m_NudgeEnabled)
        {
            ImGui::Indent();
            const char* modes[] = { "Opposite of movement input", "Along movement input", "Off the wall (uses Hook A's cached normal)" };
            if (ImGui::Combo("Nudge direction##s360", &m_NudgeDirectionMode, modes, IM_ARRAYSIZE(modes)))
                SaveSettings();
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Nudge distance##s360", &m_NudgeDistance, 0.0f, 3.0f, "%.2f m"))
                SaveSettings();
            ImGui::TextDisabled("Creates ONE extra sidehop candidate from a point "
                                "moved along/against the movement input, or off the wall.");
            ImGui::Unindent();
        }

        ImGui::Unindent();
    }
}
