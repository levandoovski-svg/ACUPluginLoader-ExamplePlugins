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

// Mechanism reference, written for reuse in future parkour plugins:
// MODLOG/sidehop-angle-widen.md - the sites/fields below in the order they must
// run, why widening the cone changes acceptance but never aiming, and a recipe.
static Sidehop360Plugin* g_Plugin = nullptr;

// ============================================================================
//  Hook A - set-ranges overrides (minCosAngle, Side_min/Max) + wall-normal
//           cache + candidate redirect (mode 2). Site inside the sidehop's
//           SetExpectedDistanceRangesAccordingToParkourMode impl.
// ============================================================================

// 0x140148500 is the sidehop's SetExpectedDistanceRangesAccordingToParkourMode
// impl (fancy vtable 0x143A13130 slot 0x64, "descent from wall to ground; also
// sidehop"). Its prologue writes the type's constants, including at
// 0x140148549 the cone `mov dword ptr [rcx+0x1e8], 0x3f3504f3` (cos 45).
// The hook sits on the NEXT instruction (0x140148553, a clean 10-byte
// `mov [rcx+0x1ec], 0x3f800000`, re-executed after the callback), so the
// callback sees minCosAngle exactly as vanilla wrote it and can override it
// before the fit chain consumes it. rcx is the action (`mov rdx,rcx`
// at the impl's entry).
// NOTE: the per-parkour-mode block LATER in this same function rewrites
// heightDifferenceMin/Max (+0x1D0/+0x1D4) - those cannot be overridden here
// (they run after this cave, via the mode jump table). Hook C handles them.
void WidenSidehopAngle_Cave(AllRegisters* params)
{
    Sidehop360Plugin* plugin = g_Plugin;
    if (!plugin || plugin->m_Mode == 0 || !plugin->ShouldWidenAngle())
        return;
    auto* action = reinterpret_cast<AvailableParkourAction*>(params->rcx_);
    if (!action)
        return;

    // Cache the wall's outward normal (XY, normalized) - used by redirect
    // sub-mode 2 and nudge direction mode 2. directionDestFacingOut (+0x40)
    // points out of the wall toward the player's face.
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

    if (plugin->m_Mode == 3)
        return; // nudge mode: Hook A is installed for the normal cache only

    plugin->m_TelLastMinCosAngleSeen = action->minCosAngle;
    action->minCosAngle = plugin->m_MinCosAngle;
    action->Side_min = plugin->m_ExpectedSideMin;
    action->Side_max = plugin->m_ExpectedSideMax;
    plugin->m_TelWidenApplications++;

    if (plugin->m_Mode != 2 || plugin->m_RedirectSubMode <= 0)
        return;

    // Redirect: re-aim the candidate's target in the ground plane. All
    // direction fields are rewritten so the candidate stays internally
    // consistent when QuickCheck / the geometry tests consume it.
    float destX, destY, dirX, dirY;
    if (plugin->m_RedirectSubMode == 1)
    {
        // Mirror 180 degrees through the src anchor.
        destX = 2.0f * action->locationAnchorSrc.x - action->locationAnchorDest.x;
        destY = 2.0f * action->locationAnchorSrc.y - action->locationAnchorDest.y;
        dirX = -action->dir80.x;
        dirY = -action->dir80.y;
    }
    else
    {
        // Off the wall: target along the wall's outward normal (toward the
        // roof across the alley), HopDistance meters out.
        destX = action->locationAnchorSrc.x + plugin->m_WallNormalX * plugin->m_HopDistance;
        destY = action->locationAnchorSrc.y + plugin->m_WallNormalY * plugin->m_HopDistance;
        dirX = plugin->m_WallNormalX;
        dirY = plugin->m_WallNormalY;
    }
    const float dirLen = sqrtf(dirX * dirX + dirY * dirY);
    if (dirLen < 1e-6f)
        return;
    dirX /= dirLen;
    dirY /= dirLen;

    action->locationAnchorDest.x = destX;
    action->locationAnchorDest.y = destY;
    action->locationAnchorDest.z = action->locationAnchorSrc.z;
    action->locationAnchorDest.w = action->locationAnchorSrc.w;
    // Direction from src to new dest: XY unit, z = 0 (dest.z == src.z), w = 0.
    action->dir80.x = dirX;  action->dir80.y = dirY;  action->dir80.z = 0.0f;  action->dir80.w = 0.0f;
    action->directionSrcToDest.x = dirX; action->directionSrcToDest.y = dirY; action->directionSrcToDest.z = 0.0f; action->directionSrcToDest.w = 0.0f;
    action->directionSrcToDestXYPlane.x = dirX; action->directionSrcToDestXYPlane.y = dirY; action->directionSrcToDestXYPlane.z = 0.0f; action->directionSrcToDestXYPlane.w = 0.0f;
    action->directionSrcToDestXYPlane_A0.x = dirX; action->directionSrcToDestXYPlane_A0.y = dirY; action->directionSrcToDestXYPlane_A0.z = 0.0f; action->directionSrcToDestXYPlane_A0.w = 0.0f;

    const float dx = destX - action->locationAnchorSrc.x;
    const float dy = destY - action->locationAnchorSrc.y;
    const float horizontalLen = sqrtf(dx * dx + dy * dy);
    action->horizontalDifference = horizontalLen;
    action->distance = horizontalLen;
    plugin->m_TelRedirects++;
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
//  Hook C - allowed-range overrides at QuickCheckAcceptableDistanceRanges
// ============================================================================

// 0x1401B5890 is QuickCheckAcceptableDistanceRanges (fancy vtable 0x143A13130
// slot 0x70) for the sidehop. It tests, with epsilonForDistanceRange
// (+0x1FC) tolerance:
//   heightDifference (+0xC0)  in [ +0x1D0 - e , +0x1D4 + e ]
//   horizontalDifference (+0xC4) in [ +0x1D8 - e , +0x1DC + e ]
//   distance (+0xC8)          in [ +0x1E0 - e , +0x1E4 + e ]
// then a dot-product cone (minCosAngle - epsilonForDotProd vs +0xCC) and more.
// NOTE: +0x1D4/+0x1DC/+0x1E4 are ABSOLUTE upper bounds, not offsets from the
// mins (verified from the comparison order: min-e <= value, value <= e+max).
// Up to v0.3.1 this cave wrote `Max - Min` into +0x1D4 because of a wrong
// "stored as offset" note, so the real ceiling was Min+Max (-10..+20 m). Now
// the sliders mean exactly what they say.
// The +0x1D0..+0x1E4 ranges are written per parkour mode at the END of
// set-ranges - AFTER Hook A's site - so only an override here (the function's
// own entry, rcx = action) survives to the checks. Entry bytes are a clean
// position-independent `push rbx; sub rsp,0x20; mov rbx,rcx` (9 bytes).
// This impl may be shared by other action types' vtables: while the hotkey is
// held those get permissive ranges too - accepted for a hold-gated test plugin.
void WidenQuickCheckRanges_Cave(AllRegisters* params)
{
    Sidehop360Plugin* plugin = g_Plugin;
    if (!plugin || (plugin->m_Mode != 1 && plugin->m_Mode != 2) || !plugin->ShouldWidenAngle())
        return;
    auto* action = reinterpret_cast<AvailableParkourAction*>(params->rcx_);
    if (!action)
        return;
    action->heightDifferenceMin = plugin->m_HeightDiffMin;
    action->heightDifferenceMax = plugin->m_HeightDiffMax;
    action->horizontalDifferenceMin = 0.05f;
    action->horizontalDifferenceMax = 30.0f;
    action->distanceMin = 0.05f;
    action->distanceMax = 40.0f;
    plugin->m_TelQuickCheckOverrides++;
}

struct SidehopQuickCheckRangesHook : AutoAssemblerCodeHolder_Base
{
    SidehopQuickCheckRangesHook()
    {
        PresetScript_CCodeInTheMiddle(
            0x1401B5890, 9,
            WidenQuickCheckRanges_Cave, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    }
};

AutoAssembleWrapper<SidehopQuickCheckRangesHook>& GetQuickCheckRangesHook()
{
    static AutoAssembleWrapper<SidehopQuickCheckRangesHook> s_hook;
    return s_hook;
}

// ============================================================================
//  Hook B - breakfall-style position nudge (mode 3)
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
    if (!plugin || plugin->m_Mode != 3 || !plugin->ShouldWidenAngle())
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

    // -- Hook A: modes 1-2 (overrides + redirect), mode 3 (cache-only). -----
    if (m_Mode != 0)
    {
        if (m_HookARefused && !IsSiteTaken(0x140148553))
            m_HookARefused = false;
        if (!m_HookAInstalled && !m_HookARefused)
        {
            if (IsSiteTaken(0x140148553))
                m_HookARefused = true;
            else
            {
                GetAngleWidenHook().Activate();
                m_HookAInstalled = true;
            }
        }
    }
    else if (m_HookAInstalled)
    {
        GetAngleWidenHook().Deactivate();
        m_HookAInstalled = false;
    }

    // -- Hook C: modes 1-2 only. --------------------------------------------
    if (m_Mode == 1 || m_Mode == 2)
    {
        if (m_HookCRefused && !IsSiteTaken(0x1401B5890))
            m_HookCRefused = false;
        if (!m_HookCInstalled && !m_HookCRefused)
        {
            if (IsSiteTaken(0x1401B5890))
                m_HookCRefused = true;
            else
            {
                GetQuickCheckRangesHook().Activate();
                m_HookCInstalled = true;
            }
        }
    }
    else if (m_HookCInstalled)
    {
        GetQuickCheckRangesHook().Deactivate();
        m_HookCInstalled = false;
    }

    // -- Hook B: mode 3 only. The site is owned by Freeform 2.0, which hooks
    //    the same climbFacade scan call site (verified live 2026-11-09: the
    //    site's E9 cave dispatches into Freeform 2.0.dll; ACUFixes.dll is not
    //    even loaded on this install). Keep retrying while the nudge mode is
    //    selected so disabling Freeform 2.0 frees the site and the hook
    //    installs on its own.
    if (m_Mode == 3)
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

    m_HooksInstalled = m_HookAInstalled || m_HookBInstalled || m_HookCInstalled;
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

        ParseInt  ("Mode=",             m_Mode);
        ParseInt  ("Hotkey=",           m_Hotkey);
        ParseFloat("MinCosAngle=",      m_MinCosAngle);
        ParseFloat("ExpectedSideMin=",  m_ExpectedSideMin);
        ParseFloat("ExpectedSideMax=",  m_ExpectedSideMax);
        ParseFloat("HeightDiffMin=",    m_HeightDiffMin);
        ParseFloat("HeightDiffMax=",    m_HeightDiffMax);
        ParseInt  ("RedirectSubMode=",  m_RedirectSubMode);
        ParseFloat("HopDistance=",      m_HopDistance);
        ParseInt  ("NudgeDirection=",   m_NudgeDirectionMode);
        ParseFloat("NudgeDistance=",    m_NudgeDistance);
    }
}

void Sidehop360Plugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (!f) return;
        f << "Mode=" << m_Mode << "\n";
        f << "Hotkey=0x" << std::hex << m_Hotkey << std::dec << "\n";
        f << "MinCosAngle=" << m_MinCosAngle << "\n";
        f << "ExpectedSideMin=" << m_ExpectedSideMin << "\n";
        f << "ExpectedSideMax=" << m_ExpectedSideMax << "\n";
        f << "HeightDiffMin=" << m_HeightDiffMin << "\n";
        f << "HeightDiffMax=" << m_HeightDiffMax << "\n";
        f << "RedirectSubMode=" << m_RedirectSubMode << "\n";
        f << "HopDistance=" << m_HopDistance << "\n";
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

    static const char* modes[] = { "Off", "Angle widen (main)", "Redirect", "Nudge" };
    ImGui::SetNextItemWidth(240);
    if (ImGui::Combo("Mode##s360", &m_Mode, modes, IM_ARRAYSIZE(modes)))
        SaveSettings();
    ImGui::TextDisabled("Modes are exclusive so each mechanism can be tested alone. "
                        "All are gated by the hold-hotkey while wall-climbing.");

    // -- Hook status -------------------------------------------------------
    if (ImGui::CollapsingHeader("Hook status", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();
        auto HookLine = [&](const char* label, bool refused, bool installed, bool usedByMode, const char* refusedMsg) {
            ImGui::Text("%s:", label);
            ImGui::SameLine();
            if (refused)
                ImGui::TextColored(inactiveColor, "%s", refusedMsg);
            else if (installed)
                ImGui::TextColored(activeColor, "INSTALLED");
            else if (usedByMode)
                ImGui::TextColored(inactiveColor, "pending...");
            else
                ImGui::TextColored(inactiveColor, "not used by this mode");
        };
        HookLine("Hook A - set-ranges (0x140148553)", m_HookARefused, m_HookAInstalled, m_Mode != 0,
            "REFUSED - site patched by another plugin");
        HookLine("Hook C - quick-check ranges (0x1401B5890)", m_HookCRefused, m_HookCInstalled, m_Mode == 1 || m_Mode == 2,
            "REFUSED - site patched by another plugin");
        HookLine("Hook B - nudge (0x140135230)", m_HookBRefused, m_HookBInstalled, m_Mode == 3,
            "REFUSED - owned by Freeform 2.0 (it hooks the same scan call site). "
            "Disable Freeform 2.0 in the loader and this installs on its own "
            "(no restart needed). Re-enabling Freeform 2.0 while the nudge is "
            "installed would break Freeform's hook.");
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
        if (m_Mode == 1 || m_Mode == 2)
        {
            ImGui::Text("Angle overrides applied: %lld", m_TelWidenApplications);
            ImGui::Text("Vanilla minCosAngle seen: %.4f (cos45 = 0.7071)", m_TelLastMinCosAngleSeen);
            ImGui::Text("QuickCheck range overrides: %lld", m_TelQuickCheckOverrides);
        }
        if (m_Mode == 2)
            ImGui::Text("Redirects applied: %lld (sub-mode %d)", m_TelRedirects, m_RedirectSubMode);
        if (m_Mode == 3)
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

        if (m_Mode == 1 || m_Mode == 2)
        {
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("minCosAngle##s360", &m_MinCosAngle, -1.0f, 1.0f, "%.3f"))
                SaveSettings();
            ImGui::SameLine();
            if (ImGui::SmallButton("360##s360")) { m_MinCosAngle = -1.0f; SaveSettings(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("vanilla##s360")) { m_MinCosAngle = 0.7071f; SaveSettings(); }
            ImGui::TextDisabled("-1 = full 360 degrees. Vanilla writes cos45 (0.7071).");

            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Expected hop min##s360", &m_ExpectedSideMin, 0.0f, 5.0f, "%.2f m"))
                SaveSettings();
            ImGui::SameLine();
            if (ImGui::SmallButton("vanilla##s360min")) { m_ExpectedSideMin = 2.0f; SaveSettings(); }
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Expected hop max##s360", &m_ExpectedSideMax, 1.0f, 15.0f, "%.2f m"))
                SaveSettings();
            ImGui::SameLine();
            if (ImGui::SmallButton("vanilla##s360max")) { m_ExpectedSideMax = 3.0f; SaveSettings(); }

            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Allowed height min##s360", &m_HeightDiffMin, -30.0f, 0.0f, "%.1f m"))
                SaveSettings();
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Allowed height max##s360", &m_HeightDiffMax, 0.0f, 30.0f, "%.1f m"))
                SaveSettings();
            ImGui::TextDisabled("Hook C: the allowed dest-height window QuickCheck accepts "
                                "(vanilla sidehop: -1.0..+2.5 m - the gate that killed "
                                "cross-alley targets). Horizontal/distance windows are "
                                "widened to 30/40 m automatically.");
        }

        if (m_Mode == 2)
        {
            ImGui::Separator();
            const char* subModes[] = { "Off (ranges only)", "Mirror 180 degrees", "Off the wall" };
            ImGui::SetNextItemWidth(240);
            if (ImGui::Combo("Redirect##s360", &m_RedirectSubMode, subModes, IM_ARRAYSIZE(subModes)))
                SaveSettings();
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Hop distance##s360", &m_HopDistance, 0.5f, 10.0f, "%.2f m"))
                SaveSettings();
            ImGui::TextDisabled("Off-the-wall places every sidehop target Hop distance out "
                                "along the wall normal (toward the roof across the alley); "
                                "Mirror flips the target to the opposite side.");
        }

        if (m_Mode == 3)
        {
            ImGui::Separator();
            const char* nudgeModes[] = { "Opposite of movement input", "Along movement input", "Off the wall (uses Hook A's cached normal)" };
            ImGui::SetNextItemWidth(240);
            if (ImGui::Combo("Nudge direction##s360", &m_NudgeDirectionMode, nudgeModes, IM_ARRAYSIZE(nudgeModes)))
                SaveSettings();
            ImGui::SetNextItemWidth(240);
            if (ImGui::SliderFloat("Nudge distance##s360", &m_NudgeDistance, 0.0f, 3.0f, "%.2f m"))
                SaveSettings();
            ImGui::TextDisabled("Creates ONE extra sidehop candidate from a point "
                                "moved off the wall or along/against the movement input.");
        }

        ImGui::Unindent();
    }
}
