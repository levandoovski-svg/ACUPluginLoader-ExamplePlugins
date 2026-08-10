#include "pch.h"
#include "AssassinationChallengePlugin.h"

#include <cstdarg>
#include <cstdio>

#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include "ACU/CSrvPlayerHealth.h"
#include "ACU/SharedPtr.h"

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"

// ---------------------------------------------------------------------------
// Game-version constants (Assassin's Creed Unity 1.5.0, UP build)
// ---------------------------------------------------------------------------

// Player state-machine "Enter" function addresses of the assassination states.
// Source: ACUFixes-master/ACUFixes/src/ParkourDebugging/LoggingTheHumanStates.cpp
// (build 1.5.0). These are Enter() addresses - they may be parent nodes in the
// state tree, so we walk the WHOLE tree, not just the leaf receiver list.
// kAssassinationState_Entry (0x141A456E0) is special: it is the Enter of
// Functor_Parkour_Assassination_Entry, the node that carries the victim
// (targetNPC @ 0x1E8) - ACUFixes-master/ACUFixes/src/VariousPatches/
// Hack_MoreReliableQuickshot.cpp:200-208, 449-454.
static const uint64_t kAssassinationStates[] = {
    0x141A498A0, // "Assassination_PP"
    0x141A456E0, // "Assassination_P" == Functor_Parkour_Assassination_Entry __Enter
    0x141A42350, // "Assassination_FirstHalf_mb"
    0x141A3FFD0, // "Assassination_SecondHalf_mb"
};
static const uint64_t kAssassinationEntryEnter = 0x141A456E0;

// The game's own "should this assassination be disallowed?" decision points.
// Same originals + callsites as ACUFixes-master
// Hack_DontForceUnsheatheWhenInDisguise.cpp (THIS build / UP 1.5.0).
// Do NOT mix in the ParryAssassinPlugin Steam 1.5.1 set.
class SharedPtr_mb;

DEFINE_GAME_FUNCTION(WhenDecidingIfAssassinationShouldBeDisallowed_Stage1,
    0x1404E6E90, char, __fastcall, (__int64 a1, SharedPtr_mb* a2, SharedPtr_mb* a3));
DEFINE_GAME_FUNCTION(WhenDecidingIfAssassinationShouldBeDisallowed_Stage2,
    0x1404E70E0, char, __fastcall, (__int64 a1, SharedPtr_mb* a2, SharedPtr_mb* a3));

// The game's per-entity "is this victim dead or in the process of dying?"
// query - the confirmed-kill gate for the desync write. Same function and same
// parameter shape ACUFixes-master's Hack_MoreReliableQuickshot.cpp:717 uses to
// exclude dead/dying NPCs from quickshot scanning. Contract #003 recon.
// Type copied from that file (size 0x18: SharedPtrNew<Entity>* shared; + 16 pad).
class SharedPtrAndSmth
{
public:
    SharedPtrNew<Entity>* shared; //0x0000
    char pad_0008[16];            //0x0008
}; //Size: 0x0018
assert_sizeof(SharedPtrAndSmth, 0x18);

DEFINE_GAME_FUNCTION(IsEntityKilledOrBeingKilled_mb,
    0x1409E6E60, char, __fastcall, (SharedPtrAndSmth* a1));

// Layout of the assassination-attempt functor, mirrored from
// Hack_MoreReliableQuickshot.cpp:200-208:
//   class Functor_Parkour_Assassination_Entry : public FunctorBase {
//       char pad_0100[0xE8];                 //0x0100..0x1E7
//       SharedPtrAndSmth targetNPC;          //0x01E8
//       SharedPtrAndSmth secondOptionalTargetNPC_mb; //0x0200  (not used here - see Q5)
//   };
struct Functor_Parkour_Assassination_EntryLayout
{
    char pad_0000[0x1E8]; //  0x0000..0x1E7
    SharedPtrAndSmth targetNPC;                 //0x01E8
    SharedPtrAndSmth secondOptionalTargetNPC_mb;//0x0200 -- reserved (recon: "when populated" unconfirmed)
}; //FunctorBase reaches 0x100; +0xE8 pad = target NPC at 0x1E8

// ---------------------------------------------------------------------------
// Entity descriptor constants (the ROOT ACU-RE Entity.h has the layout but no
// enum names; values below come from ACUFixes-master's Entity.h - identical
// layout, offsets verified: DescriptorType 0xD4, NPC = 0x02)
// ---------------------------------------------------------------------------
enum : uint32_t
{
    kNpcSubType_Templar            = 1,
    kNpcSubType_Peasant            = 2,
    kNpcSubType_Thief              = 3,
    kNpcSubType_Courtesan          = 4,
    kNpcSubType_Mercenary          = 5,
    kNpcSubType_Target             = 6, // mission assassination targets
    kNpcSubType_TemplarAchievement = 13,
    kNpcSubType_Assassin           = 15,
    kNpcSubType_Harasser           = 16,
    kNpcSubType_Animal             = 19,
    kNpcSubType_RiftNPC            = 29,
    kNpcSubType_VillaNPC           = 31,
    kNpcSubType_UniqueNPC          = 33, // story / named characters
};

static const uint32 kDescriptorTypeNPC = 0x02;

// ---------------------------------------------------------------------------
// Helpers (every game-memory read is SEH-guarded - a transient bad pointer
// must skip a frame, never crash. This mirrors ACUFixes' own plugins; see
// ParryAssassinPlugin's __try / Hack_DontForceUnsheathe.)
// ---------------------------------------------------------------------------

static float GetTimeSeconds()
{
    return (float)(GetTickCount64() / 1000.0);
}

static bool IsKnownAssassinationEnter(uint64_t enter)
{
    for (uint64_t st : kAssassinationStates)
    {
        if (enter == st)
            return true;
    }
    return false;
}

// Recursively walk the ENTIRE player state-node tree (root = HumanStatesHolder
// cast to FunctorBase*, exactly like LoggingTheHumanStates does). Hard-bounded
// to avoid infinite recursion on mutated/cyclic nodes: max depth 64, max 512
// visited nodes per call.
static bool WalkHumanStateTreeBounded(FunctorBase* node, int depth, int& budget, uint64_t& outEnter)
{
    if (!node || depth > 64 || budget <= 0)
        return false;
    --budget;

    if (IsKnownAssassinationEnter((uint64_t)node->Enter))
    {
        outEnter = (uint64_t)node->Enter;
        return true;
    }

    if (node->directChild_mb)
    {
        if (WalkHumanStateTreeBounded(node->directChild_mb, depth + 1, budget, outEnter))
            return true;
    }

    for (FunctorBase* child : node->nonoverridingChildren)
    {
        if (child && WalkHumanStateTreeBounded(child, depth + 1, budget, outEnter))
            return true;
    }
    return false;
}

// Like WalkHumanStateTreeBounded but stops at a node whose Enter equals a
// specific address and hands back the node pointer itself (needed to read the
// victim field off the assassination-entry node).
static bool FindFunctorNodeEnterBounded(FunctorBase* node, int depth, int& budget, uint64_t wantedEnter, FunctorBase*& outNode, uint64_t& outEnter)
{
    if (!node || depth > 64 || budget <= 0)
        return false;
    --budget;

    if ((uint64_t)node->Enter == wantedEnter)
    {
        outNode  = node;
        outEnter = (uint64_t)node->Enter;
        return true;
    }

    if (node->directChild_mb)
    {
        if (FindFunctorNodeEnterBounded(node->directChild_mb, depth + 1, budget, wantedEnter, outNode, outEnter))
            return true;
    }

    for (FunctorBase* child : node->nonoverridingChildren)
    {
        if (child && FindFunctorNodeEnterBounded(child, depth + 1, budget, wantedEnter, outNode, outEnter))
            return true;
    }
    return false;
}

static bool IsPlayerInAssassinationState(uint64_t& outEnter)
{
    outEnter = 0;

    __try
    {
        HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
        if (!hs)
            return false;

        FunctorBase* root = (FunctorBase*)hs;
        if (root)
        {
            int budget = 512;
            if (WalkHumanStateTreeBounded(root, 0, budget, outEnter))
                return true;
        }

        // (Leaf-node receivers - fast path, subset of the tree.)
        for (const auto& r : hs->primaryCallbackReceivers)
        {
            if (!r.pNode)
                continue;
            if (IsKnownAssassinationEnter((uint64_t)r.pNode->Enter))
            {
                outEnter = (uint64_t)r.pNode->Enter;
                return true;
            }
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static const char* SubtypeName(uint32_t st)
{
    switch (st)
    {
        case kNpcSubType_Templar:            return "Templar";
        case kNpcSubType_Peasant:            return "Peasant";
        case kNpcSubType_Thief:              return "Thief";
        case kNpcSubType_Courtesan:          return "Courtesan";
        case kNpcSubType_Mercenary:          return "Mercenary";
        case kNpcSubType_Target:             return "Target";
        case kNpcSubType_TemplarAchievement: return "TemplarAchievement";
        case kNpcSubType_Assassin:           return "Assassin";
        case kNpcSubType_Harasser:           return "Harasser";
        case kNpcSubType_Animal:             return "Animal";
        case kNpcSubType_RiftNPC:            return "RiftNPC";
        case kNpcSubType_VillaNPC:           return "VillaNPC";
        case kNpcSubType_UniqueNPC:          return "UniqueNPC";
        default:                             return "Other";
    }
}

// ---------------------------------------------------------------------------
// Plugin <-> hooks bridge
// ---------------------------------------------------------------------------
static AssassinationChallengePlugin* g_plugin = nullptr;

// The game's own decision callbacks. We only OBSERVE - call the original and
// return its result, then notify the plugin so it can start a judgement on the
// same/later frame (in OnUpdate, never re-entrant from here). SEH-guarded so a
// momentary game state never takes down the DLL.
static void OnAssassinationDecisionStage1(AllRegisters* params)
{
    char disallowed = 0;
    __try
    {
        disallowed = WhenDecidingIfAssassinationShouldBeDisallowed_Stage1(
            params->rcx_, (SharedPtr_mb*)params->rdx_, (SharedPtr_mb*)params->r8_);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        disallowed = 0;
    }
    *params->rax_ = disallowed;
    if (g_plugin)
        g_plugin->OnGameAssassinationDecision();
}

static void OnAssassinationDecisionStage2(AllRegisters* params)
{
    char disallowed = 0;
    __try
    {
        disallowed = WhenDecidingIfAssassinationShouldBeDisallowed_Stage2(
            params->rcx_, (SharedPtr_mb*)params->rdx_, (SharedPtr_mb*)params->r8_);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        disallowed = 0;
    }
    *params->rax_ = disallowed;
    if (g_plugin)
        g_plugin->OnGameAssassinationDecision();
}

struct AssassinationHookStage1 : AutoAssemblerCodeHolder_Base
{
    AssassinationHookStage1()
    {
        PresetScript_CCodeInTheMiddle(
            0x140CD96CD, 5, OnAssassinationDecisionStage1,
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, false);
    }
};

struct AssassinationHookStage2 : AutoAssemblerCodeHolder_Base
{
    AssassinationHookStage2()
    {
        PresetScript_CCodeInTheMiddle(
            0x140CD9775, 5, OnAssassinationDecisionStage2,
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, false);
    }
};

static void InstallAssassinationHooks()
{
    static AutoAssembleWrapper<AssassinationHookStage1> w1;
    static AutoAssembleWrapper<AssassinationHookStage2> w2;
    w1.Activate();
    w2.Activate();
}

// ---------------------------------------------------------------------------
// Plugin implementation
// ---------------------------------------------------------------------------

void AssassinationChallengePlugin::OnBeforeActivate()
{
    g_plugin = this;

    if (!m_HooksInstalled)
    {
        m_HooksInstalled = true;
        InstallAssassinationHooks();
    }
}

void AssassinationChallengePlugin::OnGameAssassinationDecision()
{
    if (m_Enabled)
    {
        m_HookFiredThisFrame = true;
        m_HookEventCount++;
    }
}

void AssassinationChallengePlugin::BeginJudgement(float now)
{
    m_PendingJudgement = true;
    m_PendingSince     = now;
    m_VictimCaptured   = false;
    m_Victim           = nullptr;
    m_PollCount        = 0;
    for (int i = 0; i < 0x18; ++i)
        m_VictimSharedBytes[i] = 0;
    m_VictimSince = now;
}

void AssassinationChallengePlugin::AbandonJudgement(const char* reason)
{
    AddConsoleLine("ABANDON: %s (no desync)", reason);

    LogEntry entry{};
    entry.subtype  = 0;
    entry.allowed  = false;
    entry.desynced = false;
    entry.time     = GetTimeSeconds();
    if (m_LogCount < kMaxLog)
        m_Log[m_LogCount++] = entry;
    else
    {
        for (int i = 1; i < kMaxLog; ++i)
            m_Log[i - 1] = m_Log[i];
        m_Log[kMaxLog - 1] = entry;
    }

    m_PendingJudgement = false;
    m_VictimCaptured   = false;
    m_Victim           = nullptr;
    m_PollCount        = 0;
    for (int i = 0; i < 0x18; ++i)
        m_VictimSharedBytes[i] = 0;
}

void AssassinationChallengePlugin::AddConsoleLine(const char* fmt, ...)
{
    if (!fmt)
        return;

    char line[kConsoleLineLen];
    {
        va_list va;
        va_start(va, fmt);
        vsnprintf(line, kConsoleLineLen, fmt, va);
        va_end(va);
        line[kConsoleLineLen - 1] = 0;
    }

    int dst = m_ConsoleCount; // next free ring slot
    if (dst >= kConsoleMax)
    {
        // ring full: shift down (drop oldest), stay clamped
        for (int c = 1; c < kConsoleMax; ++c)
            for (int i = 0; i < kConsoleLineLen; ++i)
                m_ConsoleLines[c - 1][i] = m_ConsoleLines[c][i];
        dst = kConsoleMax - 1;
    }
    else
    {
        ++m_ConsoleCount;
    }

    // Always write a clean, NUL-terminated line into the slot.
    for (int i = 0; i < kConsoleLineLen; ++i)
        m_ConsoleLines[dst][i] = 0;
    for (int i = 0; line[i] && i < kConsoleLineLen - 1; ++i)
        m_ConsoleLines[dst][i] = line[i];
}

// Capture the victim entity from the game's own assassination-attempt functor
// (Enter == Functor_Parkour_Assassination_Entry / 0x141A456E0), field
// targetNPC @ 0x1E8. The entire read is guarded: transient bad pointers just
// leave m_VictimCaptured false and the caller retries next frame.
void AssassinationChallengePlugin::TryCaptureVictim()
{
    m_VictimCaptured = false;

    __try
    {
        HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
        if (!hs)
            return;

        FunctorBase* root = (FunctorBase*)hs;
        if (!root)
            return;

        int        budget = 512;
        FunctorBase* node = nullptr;
        uint64_t   enter  = 0;
        if (!FindFunctorNodeEnterBounded(root, 0, budget, kAssassinationEntryEnter, node, enter) || !node)
            return;

        auto* entry = (Functor_Parkour_Assassination_EntryLayout*)node;
        if (!entry->targetNPC.shared)
            return;

        // Copy the game's SharedPtrAndSmth (24 bytes) into our member so the
        // per-frame kill query receives a stable handle even if the functor
        // node itself gets recycled by the state machine later.
        for (int i = 0; i < 0x18; ++i)
            m_VictimSharedBytes[i] = ((const char*)&entry->targetNPC)[i];

        m_Victim = entry->targetNPC.shared->GetPtr();
        if (m_Victim)
        {
            m_VictimCaptured = true;
            m_VictimSince    = GetTimeSeconds();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        m_VictimCaptured = false;
        m_Victim         = nullptr;
    }
}

char AssassinationChallengePlugin::PollVictimKilled()
{
    if (!m_VictimCaptured || !m_Victim)
        return 0;

    __try
    {
        return (char)IsEntityKilledOrBeingKilled_mb((SharedPtrAndSmth*)m_VictimSharedBytes);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1; // guarded failure -> treated as "not confirmed"
    }
}

void AssassinationChallengePlugin::OnUpdate()
{
    if (!m_Enabled)
    {
        m_InAssassination    = false;
        m_PendingJudgement   = false;
        m_HookFiredThisFrame = false;
        m_VictimCaptured     = false;
        m_Victim             = nullptr;
        m_PollCount          = 0;
        return;
    }

    const float now = GetTimeSeconds();

    // One state-machine snapshot per frame - used both for attempt DETECTION
    // and (later) for "attempt ended" detection on the confirmed-kill path.
    uint64_t   activeEnter = 0;
    const bool inAssassinationState = IsPlayerInAssassinationState(activeEnter);

    // Detector A: the game's own decision gate fired this frame (start of an
    // assassination attempt).
    if (m_HookFiredThisFrame)
    {
        m_HookFiredThisFrame = false;
        if (!m_PendingJudgement)
            BeginJudgement(now);
    }

    // Detector B: rising edge of the state-tree walk (fallback if the gate
    // never fires here).
    if (!m_PendingJudgement && inAssassinationState && !m_InAssassination)
    {
        BeginJudgement(now);
        m_ActiveEnterAddr = activeEnter;
    }
    m_InAssassination = inAssassinationState;

    if (!m_PendingJudgement)
        return;

    // ~~~ Phase 1: victim resolution (from the functor, NOT the highlight) ~~~
    if (!m_VictimCaptured)
    {
        TryCaptureVictim();

        if (!m_VictimCaptured)
        {
            if (now - m_PendingSince > kVictimResolveSec)
                AbandonJudgement("victim not resolved from assassination functor within 2.0s");
            return; // retry next frame
        }
        AddConsoleLine("Victim captured: %p (from Functor_Parkour_Assassination_Entry)", m_Victim);
    }

    // ~~~ Phase 2: descriptor classification (kept from Contract #002; applied
    // ~~~ to the functor's victim, not the highlight) ~~~
    uint32_t descriptorType = 0;
    uint32_t subtype        = 0;
    __try
    {
        descriptorType = m_Victim->EntityDescriptor_.DescriptorType;
        subtype        = m_Victim->EntityDescriptor_.SubDescriptorType;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        descriptorType = 0;
        subtype        = 0;
    }

    bool allowed = false;
    if (descriptorType == kDescriptorTypeNPC)
    {
        allowed = (m_AllowTarget && subtype == kNpcSubType_Target)
               || (m_AllowUniqueNPC && subtype == kNpcSubType_UniqueNPC);
    }

    if (allowed)
    {
        LogEntry entry{};
        entry.subtype  = subtype;
        entry.allowed  = true;
        entry.desynced = false;
        entry.time     = now;
        if (m_LogCount < kMaxLog)
            m_Log[m_LogCount++] = entry;
        else
        {
            for (int i = 1; i < kMaxLog; ++i)
                m_Log[i - 1] = m_Log[i];
            m_Log[kMaxLog - 1] = entry;
        }

        AddConsoleLine("ALLOWED victim (subtype %u %s) - no desync",
                       subtype, SubtypeName(subtype));

        m_PendingJudgement = false;
        m_VictimCaptured   = false;
        m_Victim           = nullptr;
        m_PollCount        = 0;
        return;
    }

    // ~~~ Phase 3: confirmed-kill polling ~~~
    const char killState = PollVictimKilled();
    ++m_PollCount;
    AddConsoleLine("POLL #%u victim=%p subtype=%u %s -> killState=%d",
                   m_PollCount, m_Victim, subtype, SubtypeName(subtype), (int)killState);

    if (killState > 0)
    {
        // CONFIRMED: the game's own per-entity query says the victim is dead
        // or being killed. (Timing still to be calibrated in-game - see Q4.)
        AddConsoleLine("CONFIRMED KILL victim=%p subtype=%u -> desync=%s",
                       m_Victim, subtype, m_TestMode ? "SKIPPED (TEST MODE)" : "YES");

        bool desynced = false;
        if (!m_TestMode)
        {
            __try
            {
                CSrvPlayerHealth* health = ACU::GetPlayerHealth();
                if (health)
                {
                    health->isDesynchronizationNow = 1;
                    desynced = true;
                    m_DesyncCount++;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                desynced = false;
            }
        }
        else
        {
            desynced = false; // test mode: detect + report only
        }

        LogEntry entry{};
        entry.subtype  = subtype;
        entry.allowed  = false;
        entry.desynced = desynced;
        entry.time     = now;
        if (m_LogCount < kMaxLog)
            m_Log[m_LogCount++] = entry;
        else
        {
            for (int i = 1; i < kMaxLog; ++i)
                m_Log[i - 1] = m_Log[i];
            m_Log[kMaxLog - 1] = entry;
        }

        m_PendingJudgement = false;
        m_VictimCaptured   = false;
        m_Victim           = nullptr;
        m_PollCount        = 0;
        return;
    }

    // Not yet confirmed: the attempt may still be in its kill animation, or
    // it may have been aborted (functor Exit 0x141A45890 / cancel states).
    // In both cases we keep polling until one of the three abandon conditions.
    if (!m_InAssassination)
    {
        // Player is no longer in any Assassination_* state AND the flag has
        // not flipped: the attempt ended (success path would have flipped by
        // now; this is the cancel / disallowed / missed path). Abandon.
        AbandonJudgement("assassination state exited without kill confirmation (functor Exit/cancel)");
        return;
    }

    if (now - m_VictimSince > kConfirmTimeoutSec)
    {
        AbandonJudgement("awaiting kill confirmation for 6.0s without a result");
        return;
    }
}

void AssassinationChallengePlugin::OnImGuiRender()
{
    ImGui::Checkbox("Assass. Challenge (DESYNC on non-target kill)", &m_Enabled);

    if (!m_Enabled)
        return;

    ImGui::Checkbox("Allow assassinating TARGETS (subtype Target=6)", &m_AllowTarget);
    ImGui::Checkbox("Allow assassinating UNIQUE NPCs (subtype UniqueNPC=33)", &m_AllowUniqueNPC);
    ImGui::Checkbox("Test mode: detect + log only, DO NOT desync", &m_TestMode);

    ImGui::Separator();
    ImGui::Text("Decision-gate events: %u", m_HookEventCount);
    ImGui::Text("Desyncs triggered: %u", m_DesyncCount);
    if (m_ActiveEnterAddr)
        ImGui::Text("State-machine active state: %llX", (unsigned long long)m_ActiveEnterAddr);
    ImGui::Text("Pending judgement: %s", m_PendingJudgement ? (m_VictimCaptured ? "WAITING FOR KILL CONFIRM" : "RESOLVING VICTIM") : "idle");
    if (m_Victim)
        ImGui::Text("Current victim: %p (polls: %u)", m_Victim, m_PollCount);

    ImGui::TextUnformatted("Console (newest last):");
    if (ImGui::BeginChild("assassination-challenge-console", ImVec2(0, 150), true))
    {
        constexpr int kShown = 120; // leave the scroll end without 512 draw calls per frame
        const int first = m_ConsoleCount >= kShown ? m_ConsoleCount - kShown : 0;
        for (int i = first; i < m_ConsoleCount; ++i)
            ImGui::TextUnformatted(m_ConsoleLines[i % kConsoleMax]);
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextUnformatted("Last judgements:");
    for (int i = 0; i < m_LogCount; ++i)
    {
        const LogEntry& e = m_Log[i];
        const char* verdict = e.allowed
                                  ? "ALLOWED"
                                  : (e.desynced ? "DESYNCED" : "SKIPPED");
        ImGui::Text("  %s -> %s", SubtypeName(e.subtype), verdict);
    }
}