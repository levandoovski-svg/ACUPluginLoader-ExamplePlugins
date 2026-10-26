#include "pch.h"
#include "ForcedSidehopPlugin.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cmath>
#include <cctype>

// NOTE: AvailableParkourAction::GetEnumParkourAction() is already defined in
// FreeJumpPlugin.cpp (the only other compiled GPH consumer) - do NOT redefine
// it here or the build will break with a duplicate-symbol linker error.

// Game function used to prune candidates (same address FreeJumpPlugin uses).
using SmallArray_RemoveFunc = void(__fastcall*)(void* smallArray, int p_idx, unsigned int p_elemSize);
static SmallArray_RemoveFunc SmallArray_POD__RemoveGeneric = (SmallArray_RemoveFunc)0x142726000;

// ============================================================================
//  Matching
// ============================================================================

bool ForcedSidehopPlugin::IsMatchType(int typeInt) const
{
    if (m_Params.matchOnWallNormalClimb && typeInt == (int)EnumParkourAction::onWallNormalClimb)        return true; // 65 / 0x41
    if (m_Params.matchOnWallTurnCornerOuter && typeInt == (int)EnumParkourAction::onWallTurnCornerOuter) return true; // 68 / 0x44
    for (int t : m_Params.extraActionTypes)
        if (t == typeInt) return true;
    return false;
}

// ============================================================================
//  The rebuild of the original mutation (FUN_18006d510)
// ============================================================================

static void MutateIntoSidehop(AvailableParkourAction* act, const ForcedSidehopParams& p,
                              float perpX, float perpY)
{
    // Behavior swap: point the candidate's fancy vtable at the sidehop
    // implementation (original: *(qword*)(act + 0x08) = 0x143A13130).
    act->fancyVTable = (ParkourAction_FancyVTable*)p.vtableAddress;

    // Tuned constants (original offsets noted in comments).
    act->minCosAngle             = p.minCosAngle;             // +0x1E8
    act->expectedDurationMin_mb  = p.expectedDurationMin;     // +0x210
    act->expectedHorizontalSpeed = p.expectedHorizontalSpeed; // +0x218
    act->expectedVerticalSpeed_mb = p.expectedVerticalSpeed;  // +0x220
    act->curveAllowedRangeMax    = p.curveAllowedRangeMax;    // +0x230

    // dir80 (+0x80) = the mirrored lateral (perpendicular) direction.
    // The original zeroes z and w afterwards.
    act->dir80.x = perpX;
    act->dir80.y = perpY;
    act->dir80.z = 0.0f;
    act->dir80.w = 0.0f;

    // Horizontal plane in ACU is x/y (z is up).
    const float dx = act->directionDestFacingOut.x; // +0x40
    const float dy = act->directionDestFacingOut.y; // +0x44
    const float len = sqrtf(dx * dx + dy * dy);

    // src += dir * startOffset (original: +0x10 += d * 0.5)
    act->locationAnchorSrc.x += dx * p.startOffsetMul;
    act->locationAnchorSrc.y += dy * p.startOffsetMul;

    // dest = src' + perp * reach + dir * destForward   (original: +0x30/+0x34)
    // z/w are copied from src (original: movsd qword [act+0x38], qword [act+0x18]).
    act->locationAnchorDest.x = act->locationAnchorSrc.x + perpX * p.reachMultiplier + dx * p.destForwardMul;
    act->locationAnchorDest.y = act->locationAnchorSrc.y + perpY * p.reachMultiplier + dy * p.destForwardMul;
    act->locationAnchorDest.z = act->locationAnchorSrc.z;
    act->locationAnchorDest.w = act->locationAnchorSrc.w;

    if (p.forceHeightDiff)
        act->heightDifference = p.forcedHeightDiff;   // +0xC0 (original: 0)
    act->horizontalDifference = len * p.reachMultiplier; // +0xC4
    act->distance = len * p.reachMultiplier;             // +0xC8
}

// ============================================================================
//  GPH callback - runs on the game thread inside the sort & select stage
// ============================================================================

AvailableParkourAction* ForcedSidehopPlugin::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    m_Tel.cyclesSeen++;
    m_Tel.lastCandidateCount = (int)actions.size;

    // -- Gates (same order as the original) --------------------------------
    const bool hotkeyHeld = (GetAsyncKeyState(m_Params.hotkey) & 0x8000) != 0;
    const bool dirA       = (GetAsyncKeyState(m_Params.dirKeyA) & 0x8000) != 0;
    const bool dirB       = (GetAsyncKeyState(m_Params.dirKeyB) & 0x8000) != 0;
    const bool modHeld    = (GetAsyncKeyState(m_Params.modifierKey) & 0x8000) != 0;
    const bool mouseFree  = !m_Params.blockWhenMouseButtonsHeld ||
        ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
         (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0);

    m_Tel.gateHotkey   = hotkeyHeld;
    m_Tel.gateModifier = !m_Params.requireModifier || modHeld;
    m_Tel.gateMouse    = mouseFree;

    if (!hotkeyHeld) { m_Tel.lastCycleMatched = false; return nullptr; }
    if (!m_Tel.gateModifier) { m_Tel.lastCycleMatched = false; return nullptr; }
    if (!mouseFree) { m_Tel.lastCycleMatched = false; return nullptr; }

    const bool useA = dirA || m_Params.directionOverride == 1;
    const bool useB = dirB || m_Params.directionOverride == 2;
    m_Tel.gateDirection = useA || useB;
    m_Tel.lastDirectionKey = useA ? 1 : (useB ? 2 : 0);
    if (!useA && !useB) { m_Tel.lastCycleMatched = false; return nullptr; }

    // -- Find the first matching candidate (original scans front-to-back) --
    int matchedIndex = -1;
    int matchedType = -1;
    AvailableParkourAction* chosen = nullptr;
    for (int i = 0; i < actions.size; ++i)
    {
        AvailableParkourAction* act = actions[i];
        if (!act) continue;
        const int t = (int)act->GetEnumParkourAction(); // read BEFORE mutation
        if (IsMatchType(t))
        {
            chosen = act;
            matchedIndex = i;
            matchedType = t;
            break;
        }
    }
    if (!chosen)
    {
        m_Tel.lastCycleMatched = false;
        m_Tel.lastMatchedIndex = -1;
        m_Tel.lastMatchedType = -1;
        return nullptr;
    }

    m_Tel.lastFitnessBeforeAsInt = (int)(chosen->fitness * 1000.0f);

    // -- Perpendicular direction from the held direction key ---------------
    // Original: crouch-key -> (+d.y, -d.x)   |   space-key -> (-d.y, +d.x)
    const float dx = chosen->directionDestFacingOut.x;
    const float dy = chosen->directionDestFacingOut.y;
    float perpX, perpY;
    if (useA) { perpX =  dy; perpY = -dx; }  // direction A
    else      { perpX = -dy; perpY =  dx; }  // direction B

    // -- Mutate the candidate into a forced sidehop ------------------------
    MutateIntoSidehop(chosen, m_Params, perpX, perpY);

    // -- Fitness / weight handling -----------------------------------------
    if (fabsf(chosen->fitness) < m_Params.weightFloor)
        chosen->fitness = m_Params.weightFloor;
    if (m_Params.selectionMode == 1 || m_Params.selectionMode == 2)
        chosen->fitness = m_Params.boostFitness;

    m_Tel.lastFitnessAfterAsInt = (int)(chosen->fitness * 1000.0f);

    // -- Optional: prune everything but the chosen candidate ---------------
    if (m_Params.removeOtherActions)
    {
        for (int i = actions.size - 1; i >= 0; --i)
        {
            if (actions[i] != chosen)
                SmallArray_POD__RemoveGeneric(&actions, i, sizeof(AvailableParkourAction*));
        }
    }

    m_Tel.lastCycleMatched = true;
    m_Tel.timesTriggered++;
    m_Tel.lastMatchedIndex = matchedIndex;
    m_Tel.lastMatchedType = matchedType;
    return chosen;
}

// ============================================================================
//  Hook lifecycle
// ============================================================================

void ForcedSidehopPlugin::ActivateHook()
{
    if (m_HookActive) return;
    auto gph = GenericHooksInParkourFiltering::GetSingleton();
    gph->Subscribe(m_Callbacks);
    m_HookActivator = gph->RequestGPHSortAndSelect();
    m_HookActive = true;
}

void ForcedSidehopPlugin::DeactivateHook()
{
    if (!m_HookActive) return;
    GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
    m_HookActivator.reset();
    m_HookActive = false;
}

// ============================================================================
//  Settings
// ============================================================================

std::string ForcedSidehopPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\ForcedSidehopPlugin.ini";
    return "ForcedSidehopPlugin.ini";
}

void ForcedSidehopPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        auto ParseInt    = [&](const char* key, int& out) {
            if (line.rfind(key, 0) == 0) { try { out = std::stoi(line.substr(strlen(key)), nullptr, 0); } catch (...) {} }
        };
        auto ParseFloat  = [&](const char* key, float& out) {
            if (line.rfind(key, 0) == 0) { try { out = std::stof(line.substr(strlen(key))); } catch (...) {} }
        };
        auto ParseBool   = [&](const char* key, bool& out) {
            if (line.rfind(key, 0) == 0) out = line.substr(strlen(key)) == "1";
        };

        ParseBool ("Enabled=",              m_PluginEnabled);
        ParseInt  ("Hotkey=",               m_Params.hotkey);
        ParseInt  ("DirKeyA=",              m_Params.dirKeyA);
        ParseInt  ("DirKeyB=",              m_Params.dirKeyB);
        ParseBool ("RequireModifier=",      m_Params.requireModifier);
        ParseInt  ("ModifierKey=",          m_Params.modifierKey);
        ParseBool ("BlockMouseButtons=",    m_Params.blockWhenMouseButtonsHeld);
        ParseBool ("MatchClimbNormal=",     m_Params.matchOnWallNormalClimb);
        ParseBool ("MatchTurnCornerOuter=", m_Params.matchOnWallTurnCornerOuter);

        if (line.rfind("ExtraTypes=", 0) == 0)
        {
            m_Params.extraActionTypes.clear();
            std::istringstream ss(line.substr(11));
            std::string seg;
            while (std::getline(ss, seg, ','))
            {
                if (seg.empty()) continue;
                try { m_Params.extraActionTypes.push_back(std::stoi(seg)); } catch (...) {}
            }
        }

        ParseInt  ("DirectionOverride=",    m_Params.directionOverride);

        if (line.rfind("VtableAddress=", 0) == 0)
        {
            unsigned long long v = 0;
            if (sscanf_s(line.substr(14).c_str(), "%llx", &v) == 1)
                m_Params.vtableAddress = (uintptr_t)v;
        }

        ParseFloat ("ReachMultiplier=",     m_Params.reachMultiplier);
        ParseFloat ("StartOffsetMul=",      m_Params.startOffsetMul);
        ParseFloat ("DestForwardMul=",      m_Params.destForwardMul);
        ParseFloat ("WeightFloor=",         m_Params.weightFloor);
        ParseFloat ("MinCosAngle=",         m_Params.minCosAngle);
        ParseFloat ("ExpectedDurationMin=", m_Params.expectedDurationMin);
        ParseFloat ("ExpectedHorizSpeed=",  m_Params.expectedHorizontalSpeed);
        ParseFloat ("ExpectedVertSpeed=",   m_Params.expectedVerticalSpeed);
        ParseFloat ("CurveMax=",            m_Params.curveAllowedRangeMax);
        ParseBool ("ForceHeightDiff=",      m_Params.forceHeightDiff);
        ParseFloat ("ForcedHeightDiff=",    m_Params.forcedHeightDiff);
        ParseInt  ("SelectionMode=",        m_Params.selectionMode);
        ParseFloat ("BoostFitness=",        m_Params.boostFitness);
        ParseBool ("RemoveOtherActions=",   m_Params.removeOtherActions);
    }

    RefreshExtraTypesBuf();
}

void ForcedSidehopPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (!f) return;
        f << "Enabled=" << (m_PluginEnabled ? 1 : 0) << "\n";
        f << "Hotkey=0x" << std::hex << m_Params.hotkey << std::dec << "\n";
        f << "DirKeyA=0x" << std::hex << m_Params.dirKeyA << std::dec << "\n";
        f << "DirKeyB=0x" << std::hex << m_Params.dirKeyB << std::dec << "\n";
        f << "RequireModifier=" << (m_Params.requireModifier ? 1 : 0) << "\n";
        f << "ModifierKey=0x" << std::hex << m_Params.modifierKey << std::dec << "\n";
        f << "BlockMouseButtons=" << (m_Params.blockWhenMouseButtonsHeld ? 1 : 0) << "\n";
        f << "MatchClimbNormal=" << (m_Params.matchOnWallNormalClimb ? 1 : 0) << "\n";
        f << "MatchTurnCornerOuter=" << (m_Params.matchOnWallTurnCornerOuter ? 1 : 0) << "\n";
        f << "ExtraTypes=";
        for (size_t i = 0; i < m_Params.extraActionTypes.size(); ++i)
        {
            if (i) f << ",";
            f << m_Params.extraActionTypes[i];
        }
        f << "\n";
        f << "DirectionOverride=" << m_Params.directionOverride << "\n";
        f << "VtableAddress=" << std::hex << m_Params.vtableAddress << std::dec << "\n";
        f << "ReachMultiplier=" << m_Params.reachMultiplier << "\n";
        f << "StartOffsetMul=" << m_Params.startOffsetMul << "\n";
        f << "DestForwardMul=" << m_Params.destForwardMul << "\n";
        f << "WeightFloor=" << m_Params.weightFloor << "\n";
        f << "MinCosAngle=" << m_Params.minCosAngle << "\n";
        f << "ExpectedDurationMin=" << m_Params.expectedDurationMin << "\n";
        f << "ExpectedHorizSpeed=" << m_Params.expectedHorizontalSpeed << "\n";
        f << "ExpectedVertSpeed=" << m_Params.expectedVerticalSpeed << "\n";
        f << "CurveMax=" << m_Params.curveAllowedRangeMax << "\n";
        f << "ForceHeightDiff=" << (m_Params.forceHeightDiff ? 1 : 0) << "\n";
        f << "ForcedHeightDiff=" << m_Params.forcedHeightDiff << "\n";
        f << "SelectionMode=" << m_Params.selectionMode << "\n";
        f << "BoostFitness=" << m_Params.boostFitness << "\n";
        f << "RemoveOtherActions=" << (m_Params.removeOtherActions ? 1 : 0) << "\n";
    } catch (...) {}
}

void ForcedSidehopPlugin::ResetToDefaults()
{
    m_Params = ForcedSidehopParams{};
    RefreshExtraTypesBuf();
    SaveSettings();
}

// ============================================================================
//  Per-frame update
// ============================================================================

void ForcedSidehopPlugin::Init()
{
    LoadSettings();

    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction*
    {
        return static_cast<ForcedSidehopPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.ChooseAfterSorting_fnp = nullptr;
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 4.0f; // runs slightly before FreeJumpPlugin (5.0)
    m_Callbacks.m_Name = "ForcedSidehopPlugin";
}

void ForcedSidehopPlugin::OnUpdate()
{
    // -- Key rebind capture ------------------------------------------------
    if (m_WaitingKey != KeySlot::None)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (IsRisingEdgePressed(vk))
            {
                switch (m_WaitingKey)
                {
                case KeySlot::Hotkey:   m_Params.hotkey = vk; break;
                case KeySlot::DirA:     m_Params.dirKeyA = vk; break;
                case KeySlot::DirB:     m_Params.dirKeyB = vk; break;
                case KeySlot::Modifier: m_Params.modifierKey = vk; break;
                default: break;
                }
                m_WaitingKey = KeySlot::None;
                SaveSettings();
                break;
            }
        }
        return;
    }

    // -- Hook lifecycle follows the enable flag ----------------------------
    // The hook stays installed while the plugin is enabled; the trigger keys
    // are checked inside the callback (matches the original DLL's design and
    // avoids subscribe/unsubscribe churn every frame).
    if (m_PluginEnabled && !m_HookActive)
        ActivateHook();
    else if (!m_PluginEnabled && m_HookActive)
        DeactivateHook();
}

// ============================================================================
//  ImGui debug panel
// ============================================================================

static const char* const kKeyNames[] = {
    "None", "LMB", "RMB", "Cancel", "MB4", "MB5",
    "", "", "Backspace", "Tab", "", "", "Clear", "Enter",
    "", "", "Shift", "Ctrl", "Alt", "Pause", "Caps",
    "", "", "", "", "", "", "Escape",
    "", "", "", "", "Space", "PageUp", "PageDown", "End", "Home",
    "Left", "Up", "Right", "Down",
    "", "", "", "Print", "Insert", "Delete",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "", "", "", "", "", "", "",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "LWin", "RWin", "Apps",
    "", "", "Sleep",
    "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
    "Multiply", "Add", "Separator", "Subtract", "Decimal", "Divide",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "", "", "", "", "", "", "", "",
    "NumLock", "ScrollLock",
    "", "", "", "", "", "", "", "", "", "",
    "LShift", "RShift", "LCtrl", "RCtrl", "LAlt", "RAlt", "", "",
};

static const char* KeyName(int vk)
{
    if (vk < 0 || vk >= (int)IM_ARRAYSIZE(kKeyNames)) return "Unknown";
    const char* n = kKeyNames[vk];
    return (n && n[0]) ? n : "Unknown";
}

void ForcedSidehopPlugin::RefreshExtraTypesBuf()
{
    std::string s;
    for (size_t i = 0; i < m_Params.extraActionTypes.size(); ++i)
    {
        if (i) s += ",";
        s += std::to_string(m_Params.extraActionTypes[i]);
    }
    strncpy_s(m_ExtraTypesBuf, s.c_str(), _TRUNCATE);
}

void ForcedSidehopPlugin::OnImGuiRender()
{
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
    const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);

    ImGui::Text("Forced Sidehop (rebuilt from ACU_ParkourPatches)");
    ImGui::Separator();
    ImGui::TextWrapped("While wall-climbing, hold the Hotkey (+ Space/Ctrl for direction) to convert "
                       "the next wall-climb/corner candidate into an immediate sideways hop. "
                       "All original constants are tunable below.");

    bool enabled = m_PluginEnabled;
    if (ImGui::Checkbox("Enable Forced Sidehop", &enabled))
    {
        m_PluginEnabled = enabled;
        SaveSettings();
    }

    // -- Live telemetry ----------------------------------------------------
    if (ImGui::CollapsingHeader("Live Telemetry", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        ImGui::Text("Hook state:");
        ImGui::SameLine();
        ImGui::TextColored(m_HookActive ? activeColor : inactiveColor,
                           m_HookActive ? "INSTALLED" : "not installed");

        const bool triggered = m_Tel.lastCycleMatched.load();
        ImGui::Text("Last parkour cycle:");
        ImGui::SameLine();
        if (triggered)
            ImGui::TextColored(activeColor, "SIDEHOP FORCED");
        else
            ImGui::TextColored(inactiveColor, "no match / declined");

        ImGui::Text("Cycles seen: %lld    Times triggered: %lld",
                    (long long)m_Tel.cyclesSeen.load(), (long long)m_Tel.timesTriggered.load());
        ImGui::Text("Candidates last cycle: %d    Matched index: %d",
                    m_Tel.lastCandidateCount.load(), m_Tel.lastMatchedIndex.load());

        const int t = m_Tel.lastMatchedType.load();
        ImGui::Text("Matched type: %d (%s)", t,
            t >= 0 ? enum_reflection<EnumParkourAction>::GetString((EnumParkourAction)t) : "-");

        ImGui::Text("Fitness: %8.3f -> %8.3f",
                    m_Tel.lastFitnessBeforeAsInt.load() / 1000.0f,
                    m_Tel.lastFitnessAfterAsInt.load() / 1000.0f);

        // Gate chips
        auto Gate = [&](const char* label, bool ok) {
            ImGui::TextColored(ok ? activeColor : ImVec4(0.55f, 0.55f, 0.55f, 1.0f),
                               ok ? "[x] %s" : "[ ] %s", label);
        };
        ImGui::Text("Trigger gates this cycle:");
        Gate("Hotkey held", m_Tel.gateHotkey.load());
        Gate("Direction key (A/B)", m_Tel.gateDirection.load());
        Gate("Modifier ok", m_Tel.gateModifier.load());
        Gate("Mouse buttons free", m_Tel.gateMouse.load());
        if (m_Tel.lastDirectionKey.load())
            ImGui::Text("Direction used: %s", m_Tel.lastDirectionKey.load() == 1 ? "A (perp +)" : "B (perp -)");

        ImGui::Unindent();
    }

    // -- Trigger conditions ------------------------------------------------
    if (ImGui::CollapsingHeader("Trigger Conditions", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        auto KeyBindRow = [&](const char* label, int& key, KeySlot slot)
        {
            ImGui::Text("%s:", label);
            ImGui::SameLine(120);
            if (m_WaitingKey == slot)
            {
                ImGui::TextColored(rebindColor, "press any key...");
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel"))
                    m_WaitingKey = KeySlot::None;
            }
            else if (ImGui::Button((std::string(KeyName(key)) + "##" + label).c_str()))
                m_WaitingKey = slot;
        };

        KeyBindRow("Hotkey (hold)",        m_Params.hotkey,    KeySlot::Hotkey);
        KeyBindRow("Direction A",          m_Params.dirKeyA,   KeySlot::DirA);
        ImGui::TextDisabled("    A = perp (+d.y, -d.x) - original crouch-key branch");
        KeyBindRow("Direction B",          m_Params.dirKeyB,   KeySlot::DirB);
        ImGui::TextDisabled("    B = perp (-d.y, +d.x) - original space-key branch");

        bool reqMod = m_Params.requireModifier;
        if (ImGui::Checkbox("Require modifier key", &reqMod))
        {
            m_Params.requireModifier = reqMod;
            SaveSettings();
        }
        if (m_Params.requireModifier)
        {
            ImGui::Indent();
            KeyBindRow("Modifier key", m_Params.modifierKey, KeySlot::Modifier);
            ImGui::Unindent();
        }

        bool blockMouse = m_Params.blockWhenMouseButtonsHeld;
        if (ImGui::Checkbox("Decline while LMB/RMB held (original behavior)", &blockMouse))
        {
            m_Params.blockWhenMouseButtonsHeld = blockMouse;
            SaveSettings();
        }

        const char* dirModes[] = { "From keys (A/B)", "Force direction A", "Force direction B" };
        if (ImGui::Combo("Direction source", &m_Params.directionOverride, dirModes, IM_ARRAYSIZE(dirModes)))
            SaveSettings();

        ImGui::Unindent();
    }

    // -- Target actions ----------------------------------------------------
    if (ImGui::CollapsingHeader("Target Actions", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        bool m1 = m_Params.matchOnWallNormalClimb;
        if (ImGui::Checkbox("onWallNormalClimb (65 / 0x41)", &m1))
        { m_Params.matchOnWallNormalClimb = m1; SaveSettings(); }

        bool m2 = m_Params.matchOnWallTurnCornerOuter;
        if (ImGui::Checkbox("onWallTurnCornerOuter (68 / 0x44)", &m2))
        { m_Params.matchOnWallTurnCornerOuter = m2; SaveSettings(); }

        ImGui::TextDisabled("The first matching candidate in the list is chosen "
                            "(original scans front-to-back).");

        ImGui::Text("Extra TypeInts (comma separated):");
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputText("##extraTypes", m_ExtraTypesBuf, sizeof(m_ExtraTypesBuf), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            m_Params.extraActionTypes.clear();
            std::istringstream ss(m_ExtraTypesBuf);
            std::string seg;
            while (std::getline(ss, seg, ','))
            {
                if (seg.empty()) continue;
                try { m_Params.extraActionTypes.push_back(std::stoi(seg)); } catch (...) {}
            }
            SaveSettings();
        }

        ImGui::Unindent();
    }

    // -- Mutation parameters -----------------------------------------------
    if (ImGui::CollapsingHeader("Mutation Parameters", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        auto DragF = [&](const char* label, float* v, float speed, float mn, float mx, const char* fmt = "%.3f")
        {
            if (ImGui::DragFloat(label, v, speed, mn, mx, fmt))
                SaveSettings();
        };

        ImGui::TextWrapped("Reach / geometry:");
        DragF("Reach multiplier",   &m_Params.reachMultiplier, 0.05f, 0.0f, 20.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(orig 3.5)");
        DragF("Start offset (src += dir*x)", &m_Params.startOffsetMul, 0.01f, 0.0f, 5.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(orig 0.5)");
        DragF("Dest forward (dest += dir*x)", &m_Params.destForwardMul, 0.01f, 0.0f, 5.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(orig 0.5)");

        ImGui::Separator();
        ImGui::TextWrapped("Weight / fitness:");
        DragF("Weight floor",       &m_Params.weightFloor, 0.05f, 0.0f, 100.0f, "%.2f");
        ImGui::SameLine(); ImGui::TextDisabled("(orig 1.0)");

        const char* selModes[] = { "Floor only (original-faithful)", "Boost fitness (this framework)", "Floor + Boost" };
        if (ImGui::Combo("Selection mode", &m_Params.selectionMode, selModes, IM_ARRAYSIZE(selModes)))
            SaveSettings();
        if (m_Params.selectionMode == 1 || m_Params.selectionMode == 2)
        {
            DragF("Boost fitness value", &m_Params.boostFitness, 10.0f, 0.0f, 100000.0f, "%.1f");
        }
        bool rm = m_Params.removeOtherActions;
        if (ImGui::Checkbox("Remove all other candidates (guaranteed pick)", &rm))
        { m_Params.removeOtherActions = rm; SaveSettings(); }

        ImGui::Separator();
        ImGui::TextWrapped("Candidate tuning (written into the chosen action):");
        DragF("minCosAngle (+0x1E8)",   &m_Params.minCosAngle, 0.005f, -1.0f, 1.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(orig 0.7071)");
        DragF("Expected duration (+0x210)", &m_Params.expectedDurationMin, 0.01f, 0.0f, 10.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(orig 0.5)");
        DragF("Expected horiz speed (+0x218)", &m_Params.expectedHorizontalSpeed, 0.1f, 0.0f, 30.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(orig 4.0)");
        DragF("Expected vert speed (+0x220)",  &m_Params.expectedVerticalSpeed, 0.1f, 0.0f, 40.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(orig 9.0)");
        DragF("Curve allowed max (+0x230)",    &m_Params.curveAllowedRangeMax, 0.05f, -10.0f, 20.0f);
        ImGui::SameLine(); ImGui::TextDisabled("(orig 2.0)");

        bool fh = m_Params.forceHeightDiff;
        if (ImGui::Checkbox("Force heightDifference (+0xC0)", &fh))
        { m_Params.forceHeightDiff = fh; SaveSettings(); }
        if (m_Params.forceHeightDiff)
        {
            ImGui::Indent();
            DragF("Forced height diff", &m_Params.forcedHeightDiff, 0.05f, -20.0f, 20.0f);
            ImGui::SameLine(); ImGui::TextDisabled("(orig 0.0)");
            ImGui::Unindent();
        }

        ImGui::Separator();
        ImGui::TextWrapped("Fancy vtable (behavior swap):");
        unsigned long long vaddr = (unsigned long long)m_Params.vtableAddress;
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputScalar("##vtableAddr", ImGuiDataType_U64, &vaddr, nullptr, nullptr, "%llX"))
        {
            m_Params.vtableAddress = (uintptr_t)vaddr;
            SaveSettings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(orig 0x143A13130)");
        ImGui::TextDisabled("horizontalDifference / distance are always len * reachMultiplier.");

        ImGui::Unindent();
    }

    // -- Reset -------------------------------------------------------------
    ImGui::Separator();
    if (ImGui::Button("Reset to Defaults", ImVec2(160, 0)))
    {
        ResetToDefaults();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Restores every parameter and keybind to the reverse-engineered defaults.");
}
