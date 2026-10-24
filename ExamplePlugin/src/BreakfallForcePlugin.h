#pragma once
#include <string>

// BreakfallForcePlugin v2
//
// Design grounded in the runtime dump of the user's ACU.exe
// (MODLOG/breakfall-gate.md, sha256 748abdc6...; no ASLR, base 0x140000000):
//
//   * The breakfall gate function 0x141A3C0A3..0x141A3C24B contains, at
//     0x141A3C0E1, a 5-byte Denuvo VM thunk (E9 3A 40 3F FE -> 0x13FE30120).
//     Its fall-through runs the validity chain and, on success, requests the
//     follow-up parkour action (ECX=0x21 -> CALL 0x140C85280).
//   * The tail thunk at 0x141A3C293 (E9 88 3E 3E FE -> 0x13FE20120) closes the
//     notify path. Both thunks are exactly the two sites ACUFixes-0.9.3's
//     Hack_BreakfallAndLedgeGrab steals (PresetScript_CCodeInTheMiddle, 5 bytes).
//   * Mode 1 replicates that library-proven assist: nudge the feet-position
//     vector (RBP) toward the movement direction (R14) while the force key is
//     held, restore it in the epilogue hook.
//   * Mode 2 is the aggressive "regardless of active move" variant: NOP the
//     VM thunks (and optionally the height-check JBE at 0x141A3C21D) so the
//     gate's own allow-path always runs and the game itself drives the
//     transition + animation through its normal request pipeline
//     (0x140C85290 -> graph trigger 0x1B3 posted by 0x141A3BB20).
//
// NOTE (tester-reported): ForceFreefallPlugin / GroundRollPlugin (state-enter
// RAX redirect via SharedStateEnterDispatcher) are FAILED attempts and are not
// used here. Their failure is consistent with the dump: they bypassed the
// request pipeline that actually triggers the animation graph.
class BreakfallForcePlugin {
public:
    void LoadSettings();
    void Init();            // call once when code patches are safe to apply
    void OnUpdate();
    void OnImGuiRender();

private:
    // settings
    int           m_Mode            = 1;      // 0=off, 1=hook-assist, 2=force-nop
    unsigned char m_ForceKey        = 0x54;   // 'T'
    bool          m_AlwaysOn        = false;  // mode 2: force without holding key
    float         m_PositionNudge   = 1.0f;   // mode 1: pretend-closer multiplier
    bool          m_SkipHeightCheck = true;   // mode 2: also NOP height-check JBE

    // runtime state
    bool m_HookInstalled   = false;
    bool m_PatchesApplied  = false;
    bool m_PrevKeyDown     = false;

    unsigned char m_OrigGate[5]     = {};
    unsigned char m_OrigEpilogue[5] = {};
    unsigned char m_OrigHeightJbe[2] = {};

    std::string GetIniPath() const;
    std::string GetLogPath() const;
    void Log(const std::string& line);
    void PushSettingsToHooks();
    bool ApplyBytePatches();
    void RestoreBytePatches();
};
