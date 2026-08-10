#pragma once

// ---------------------------------------------------------------------------
// AssassinationChallengePlugin
// ---------------------------------------------------------------------------
// "Only assassinate targets" challenge mod for Assassin's Creed Unity 1.5.0
// (address set: ACUFixes-master / Uplay, NOT the Steam 1.5.1 Parry set).
//
// Rule: performing an assassination on any NPC that is NOT a mission
// "Target" (EntityDescriptorNPCSubType Target = 6) or a story UniqueNPC (=33)
// desynchronizes the player (game over). Assassinating a real target is the
// only permitted kill of this kind.
//
// Detection (two independent paths) - decides when a judgement is STARTED:
//   A. Hook the game's OWN decision point:
//      WhenDecidingIfAssassinationShouldBeDisallowed (callsites 0x140CD96CD /
//         0x140CD9775, originals 0x1404E6E90 / 0x1404E70E0) - the same two
//         callsites ACUFixes-master's Hack_DontForceUnsheatheWhenInDisguise
//         uses on this exact build. Fires the instant the player attempts an
//         assassination, guaranteed before the kill animation.
//   B. Fallback: walk the ENTIRE HumanStatesHolder node tree (directChild_mb
//         + nonoverridingChildren, like LoggingTheHumanStates does) checking
//         every node's Enter() address against the Assassination_* states.
//
// CONFIRMED-KILL GATING (Contract #004):
//   The judgement does NOT desync at attempt time. Once an assassination
//   attempt is detected, the victim is captured from the game's own
//   assassination-attempt functor (Functor_Parkour_Assassination_Entry,
//   Enter 0x141A456E0), field targetNPC @ 0x1E8 - NOT from the target-highlight
//   (BhvAssassin->toHighlightedNPC->highlightedNPC), because the functor's own
//   victim pointer is the one proven correct by ACUFixes-master's
//   Hack_MoreReliableQuickshot for this exact purpose.
//   The desync write fires ONLY when the game's own per-entity query
//   IsEntityKilledOrBeingKilled_mb (0x1409E6E60) returns true for that victim
//   (confirmed kill / being killed), or the attempt ends (state walk shows
//   the player is no longer in an Assassination_* state = functor Exit
//   0x141A45890 or a cancellation) with no confirmation, or kConfirmTimeoutSec
//   elapses - in which cases we abandon WITHOUT desyncing.
//
//   SAFE DEFAULTS (Contract #004): the master switch m_Enabled defaults to
//   FALSE. The mod must be explicitly enabled in the ImGui menu or it does
//   nothing at all. This is deliberate: a mod whose failure mode is silently
//   ending the player's run must not be armed on load.
//
// SAFETY: the player state tree is mutated by the game every frame. Like
// ACUFixes' own plugins (see ParryAssassinPlugin's __try, or
// Hack_DontForceUnsheathe), every read of game memory here is wrapped in
// low-level guarded sections; any transient bad pointer = one skipped frame,
// never a crash. Walk width is hard-bounded to 512 nodes / depth 64.
//
// Desync: writes CSrvPlayerHealth::isDesynchronizationNow = 1 (the same
// field Cheat_Health.cpp toggles - proven on this build).
// ---------------------------------------------------------------------------

class Entity;

class AssassinationChallengePlugin
{
public:
    void OnBeforeActivate();
    void OnUpdate();
    void OnImGuiRender();

    // Called from the game-thread code cave when the game decides about an
    // assassination attempt (detector A).
    void OnGameAssassinationDecision();

private:
    bool m_Enabled        = false;  // master switch - SAFE DEFAULT: OFF
    bool m_AllowTarget    = true;   // EntityDescriptorNPC_Target (6)
    bool m_AllowUniqueNPC = true;   // EntityDescriptorNPC_UniqueNPC (33)
    bool m_TestMode       = false;  // log-only: detect + report, never desync

    // detector A state
    bool     m_HooksInstalled     = false;
    bool     m_HookFiredThisFrame = false;
    uint32_t m_HookEventCount     = 0;

    // detector B state / current-frame snapshot (also used to detect the
    // attempt functor Exit / cancel = not in an Assassination_* state)
    bool     m_InAssassination = false;
    uint64_t m_ActiveEnterAddr = 0;

    // shared pending-judgement state
    bool     m_PendingJudgement = false;
    float    m_PendingSince     = 0.0f;
    uint32_t m_DesyncCount      = 0;

    // confirmed-kill gating state (Contract #004)
    bool    m_VictimCaptured = false;                // victim resolved from the functor
    Entity* m_Victim         = nullptr;              // victim entity (descriptor classification)
    alignas(8) char m_VictimSharedBytes[0x18];       // copy of game's SharedPtrAndSmth (victim); 8-aligned because the game reads a pointer at offset 0
    float   m_VictimSince    = 0.0f;                 // when the victim was captured
    uint32_t m_PollCount     = 0;                    // polls issued for the current victim

    // windows for the two abandon conditions (safety windows)
    // kVictimResolveSec    = max time to find the victim after the attempt starts (2.0s)
    // kConfirmTimeoutSec   = max time to AWAIT the kill-confirm after victim capture (6.0s)
    static constexpr float kVictimResolveSec  = 2.0f;
    static constexpr float kConfirmTimeoutSec = 6.0f;

    // in-plugin ImGui console logger (there is no shared console API in the
    // tree - Hack_MoreReliableQuickshot uses its own data too). Rolling ring
    // buffer; large enough that the CONFIRMED/ABANDON milestone lines survive
    // the per-frame poll spam long enough for a human to read in-game.
    static constexpr int kConsoleMax = 512;
    static constexpr int kConsoleLineLen = 128;
    char  m_ConsoleLines[kConsoleMax][kConsoleLineLen];
    int   m_ConsoleCount = 0;

    // helpers (defined in .cpp)
    void AddConsoleLine(const char* fmt, ...);
    void TryCaptureVictim();   // resolve m_Victim/m_VictimSharedBytes from the functor
    char PollVictimKilled();   // IsEntityKilledOrBeingKilled_mb(m_VictimSharedBytes), guarded
    void BeginJudgement(float now);
    void AbandonJudgement(const char* reason);

    struct LogEntry
    {
        uint32_t subtype;
        bool     allowed;
        bool     desynced;
        float    time;
    };
    static constexpr int kMaxLog = 8;
    LogEntry m_Log[kMaxLog];
    int      m_LogCount = 0;
};