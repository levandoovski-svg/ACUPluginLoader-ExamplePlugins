#pragma once
#include <string>

// BreakfallForcePlugin v3
//
// PURPOSE (per tester): the game only allows breakfall from a freefall state.
// The plugin must FORCE the player into the freefall state regardless of what
// the game thinks the current state is. T = cancel current state -> freefall.
//
// Evidence base: runtime dump of the user's ACU.exe (MODLOG/breakfall-gate.md,
// sha256 748abdc6..., no ASLR, base 0x140000000) + typed structs from
// CommonLibACU (HumanStatesHolder / FunctorBase / SmallArraySemistatic).
//
// Verified mechanics used here:
//  * InUnsafeJump (freefall) enter = 0x141A3DFD0 (thunk into real body).
//    Its body, run on a state node: sets [parentStack[0] + 0x294] = 1 (freefall
//    flag on the shared parent), clears/sets motion flags 0x27/0x28 through the
//    holder's integerSignalReceiversManager (+0x1CD0), writes position into
//    node+0x1D0..0x1DC, sets float params 0.7f/0.4f, and sets atom-graph vars
//    by hash: 0xBDEC3825 := 123, 0x4D509A44 := 0 (graph follows into freefall).
//  * State-cancel primitive = 0x141A3E4C0(parentNode): the exact call the
//    breakfall gate makes before requesting its transition. Kills the current
//    child state of the parent and installs a fresh node (always returns 1).
//  * The breakfall gate itself = 0x141A3C0A3..0x141A3C24B with the Denuvo VM
//    thunk at 0x141A3C0E1 (E9 3A 40 3F FE) and its tail thunk at 0x141A3C293.
//    DANGER: the tail thunk is the LAST instruction of its block — it must
//    NEVER be NOPed (would fall off the end). Only ever hook it (mode 1).
//
// Modes:
//   0 = off
//   1 = hook-assist (ACUFixes-0.9.3 ledge-grab nudge; only has an effect while
//       ALREADY freefalling near a wall — it cannot force the state)
//   2 = nop the gate's freefall test (gate thunk [+ optional height JBE])
//   3 = force freefall on keypress (default): run the game's own freefall enter
//       on the player's current state node, optionally preceded by the game's
//       own state-cancel primitive.
class BreakfallForcePlugin {
public:
    void LoadSettings();
    void Init();            // call once when code patches are safe to apply
    void OnUpdate();
    void OnImGuiRender();

private:
    // settings
    int           m_Mode            = 3;      // see modes above
    unsigned char m_ForceKey        = 0x54;   // 'T'
    bool          m_AlwaysOn        = false;  // mode 2: patch without holding key
    bool          m_StateReset      = true;   // mode 3: also run 0x141A3E4C0 cancel primitive
    float         m_PositionNudge   = 1.0f;   // mode 1
    bool          m_SkipHeightCheck = true;   // mode 2: also NOP the height-check JBE

    // runtime state
    bool m_HookInstalled   = false;
    bool m_PatchesApplied  = false;
    bool m_PrevKeyDown     = false;

    unsigned char m_OrigGate[5]      = {};
    unsigned char m_OrigHeightJbe[2] = {};

    std::string GetIniPath() const;
    std::string GetLogPath() const;
    void Log(const std::string& line);
    void PushSettingsToHooks();
    bool EnsureDefaultIni();
    bool ApplyBytePatches();
    void RestoreBytePatches();
    bool ForceFreefallNow();
};
