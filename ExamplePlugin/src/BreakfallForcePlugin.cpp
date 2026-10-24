#include "pch.h"
#include "BreakfallForcePlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/HumanStatesHolder.h"
#include "vmath/vmath.h"
#include <shlobj.h>
#include <cstdio>

// ── Addresses verified against the runtime dump of the user's ACU.exe ────────
// MODLOG/breakfall-gate.md (sha256 748abdc6456397fe...; no ASLR, base 0x140000000).
// The ACUFixes 1.5.1 addresses (0x141A3B4D1 / 0x141A3B683) belong to a different
// game build (0x141A3B683 is not an instruction boundary here) — do not use.
static constexpr uintptr_t ADDR_GATE_VMTHUNK   = 0x141A3C0E1; // E9 3A 40 3F FE -> 0x13FE30120 (Denuvo VM)
static constexpr uintptr_t ADDR_EPILOGUE_VMTHUNK = 0x141A3C293; // E9 88 3E 3E FE -> 0x13FE20120 (TAIL thunk — hook only, NEVER NOP)
static constexpr uintptr_t ADDR_HEIGHT_JBE     = 0x141A3C21D; // 76 0A : COMISS height-check -> epilogue on <=
static constexpr uintptr_t ADDR_FREEFALL_ENTER = 0x141A3DFD0; // InUnsafeJump enter (thunk into real body)
static constexpr uintptr_t ADDR_STATE_CANCEL   = 0x141A3E4C0; // kill current child state of a parent node (game's own primitive)

// ── Settings mirrored for the AutoAssemblerKinda callbacks (plain functions) ─
static bool     s_ForcedActive  = false;
static float    s_PositionNudge = 1.0f;
static Vector4f s_SavedFeetPos  {};

// diagnostics
static volatile long s_GateHookHits    = 0;
static volatile long s_EpilogueHookHits = 0;
static volatile long s_ForcedCount      = 0;
static uint64_t      s_LastChosenEnter  = 0;

static bool IsKeyDown(unsigned char vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// SEH-safe one-arg call into game code (no C++ objects in this frame).
static bool SafeCall1(uintptr_t fn, void* arg)
{
    __try {
        ((void(__fastcall*)(void*))fn)(arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Mode 1: library-proven assist hooks (ACUFixes-0.9.3 Hack_BreakfallAndLedgeGrab) ──
// Steals the 5-byte VM thunks, runs our callback, then re-executes the stolen
// JMP so the virtualized test itself still runs — with our tweaked input.
// NOTE: this only affects the ledge-scan WHILE the gate runs (freefall); it
// cannot force the state. Kept for completeness / wall-fall assistance.
static void Hook_GateTest_Assist(AllRegisters* params)
{
    InterlockedIncrement(&s_GateHookHits);
    Vector4f* feetPos = (Vector4f*)params->rbp_;
    s_SavedFeetPos = *feetPos;
    if (!s_ForcedActive)
        return;
    // Component-wise: this vmath build has no Vector4f+Vector3f operator.
    Vector3f* moveDir = (Vector3f*)params->r14_;
    feetPos->x += moveDir->x * s_PositionNudge;
    feetPos->y += moveDir->y * s_PositionNudge;
    feetPos->z += moveDir->z * s_PositionNudge;
}

static void Hook_GateEpilogue_Assist(AllRegisters* params)
{
    InterlockedIncrement(&s_EpilogueHookHits);
    // Restore before the modified vector feeds the "bump against wall" test.
    *(Vector4f*)params->rbp_ = s_SavedFeetPos;
}

struct GateAssistHook : AutoAssemblerCodeHolder_Base
{
    GateAssistHook()
    {
        PresetScript_CCodeInTheMiddle(ADDR_GATE_VMTHUNK, 5,
            Hook_GateTest_Assist, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
        PresetScript_CCodeInTheMiddle(ADDR_EPILOGUE_VMTHUNK, 5,
            Hook_GateEpilogue_Assist, RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    }
};

// ── Mode 2: reversible byte patches (gate test only — see DANGER note above) ─
static bool WriteBytes(uintptr_t addr, const unsigned char* bytes, size_t len)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    memcpy((void*)addr, bytes, len);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    DWORD ignored = 0;
    VirtualProtect((void*)addr, len, oldProtect, &ignored);
    return true;
}

static const unsigned char NOP5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
static const unsigned char NOP2[2] = { 0x90, 0x90 };

// ── Mode 3: force freefall using the game's own state machinery ─────────────
bool BreakfallForcePlugin::ForceFreefallNow()
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) {
        Log("ForceFreefall: HumanStatesHolder::GetForPlayer() == null (not spawned yet?)");
        return false;
    }

    // Pick the current active state node: prefer a parkour-family node
    // (Enter address in the 0x141A3xxxx parkour region), else any node that
    // has a parent. All state nodes share FunctorBase (parentStack at +0x28)
    // and the enter only touches shared parents + node+0x1D0..0x1EC.
    FunctorBase* chosen = nullptr;
    FunctorBase* fallback = nullptr;
    for (auto& r : hs->primaryCallbackReceivers) {
        FunctorBase* n = r.pNode;
        if (!n) continue;
        uint64_t e = (uint64_t)n->Enter;
        if (e >= 0x141A3B000ull && e <= 0x141A3F000ull) { chosen = n; break; }
        if (!fallback && n->parentStack.size >= 1) fallback = n;
    }
    if (!chosen) chosen = fallback;
    if (!chosen || chosen->parentStack.size < 1) {
        Log("ForceFreefall: no usable active state node found in primaryCallbackReceivers");
        return false;
    }
    s_LastChosenEnter = (uint64_t)chosen->Enter;

    FunctorBase* parentNode = chosen->parentStack.arr[0];
    if (!parentNode) {
        Log("ForceFreefall: parentStack[0] == null");
        return false;
    }

    // 1) Run the game's own freefall (InUnsafeJump) enter on the node.
    //    Sets the freefall flag on the shared parent (+0x294 = 1), motion
    //    flags 0x27/0x28 via the holder, position into node+0x1D0.., float
    //    params 0.7/0.4, and graph vars 0xBDEC3825:=123 / 0x4D509A44:=0 —
    //    the animation graph follows into freefall.
    if (!SafeCall1(ADDR_FREEFALL_ENTER, chosen)) {
        Log("ForceFreefall: freefall enter (0x141A3DFD0) faulted (SEH)");
        return false;
    }

    // 2) Optionally run the game's own state-cancel primitive — the exact call
    //    the breakfall gate makes before requesting its transition. Kills the
    //    current child state of the parent and installs a fresh node.
    if (m_StateReset) {
        if (!SafeCall1(ADDR_STATE_CANCEL, parentNode)) {
            Log("ForceFreefall: state-cancel (0x141A3E4C0) faulted (SEH)");
        }
    }

    InterlockedIncrement(&s_ForcedCount);
    char enterBuf[32];
    snprintf(enterBuf, sizeof(enterBuf), "%llX", (unsigned long long)s_LastChosenEnter);
    Log(std::string("ForceFreefall: freefall enter ran (node Enter=0x") + enterBuf
        + (m_StateReset ? ", state-cancel ran)" : ")"));
    return true;
}

// ── Plugin ───────────────────────────────────────────────────────────────────
std::string BreakfallForcePlugin::GetIniPath() const
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\BreakfallForce.ini";
    return "BreakfallForce.ini";
}

std::string BreakfallForcePlugin::GetLogPath() const
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\BreakfallForce.log";
    return "BreakfallForce.log";
}

void BreakfallForcePlugin::Log(const std::string& line)
{
    try {
        std::ofstream f(GetLogPath(), std::ios::app);
        if (f) f << "[BreakfallForce] " << line << "\n";
    } catch (...) {}
    OutputDebugStringA(("[BreakfallForce] " + line + "\n").c_str());
}

// The ini was never created before — that's why it "couldn't be found".
// Create the folder + a commented default ini on first run.
bool BreakfallForcePlugin::EnsureDefaultIni()
{
    const std::string path = GetIniPath();
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return true; // exists
    try {
        auto parent = std::filesystem::path(path).parent_path();
        std::filesystem::create_directories(parent);
        std::ofstream f(path);
        if (!f) return false;
        f << "// BreakfallForce configuration (created on first run)\n"
          << "// Mode: 0=off, 1=hook-assist (ledge-grab nudge, freefall only),\n"
          << "//       2=nop gate freefall-test, 3=force freefall on keypress (recommended)\n"
          << "Mode=3\n"
          << "// Virtual-key code (hex) of the force key. 54 = 'T'\n"
          << "ForceKey=54\n"
          << "// Mode 2 only: keep the patch applied without holding the key\n"
          << "AlwaysOn=0\n"
          << "// Mode 3 only: also run the game's own state-cancel primitive\n"
          << "StateReset=1\n"
          << "// Mode 1 only: ledge-scan nudge multiplier\n"
          << "PositionNudge=1.0\n"
          << "// Mode 2 only: also NOP the height-check JBE (0x141A3C21D)\n"
          << "SkipHeightCheck=1\n";
        return true;
    } catch (...) {
        return false;
    }
}

void BreakfallForcePlugin::PushSettingsToHooks()
{
    s_PositionNudge = m_PositionNudge;
}

void BreakfallForcePlugin::LoadSettings()
{
    EnsureDefaultIni();
    std::ifstream f(GetIniPath());
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            auto startsWith = [&line](const char* k) { return line.rfind(k, 0) == 0; };
            try {
                if (startsWith("Mode="))                m_Mode = std::stoi(line.substr(5));
                else if (startsWith("ForceKey="))       m_ForceKey = (unsigned char)std::stoi(line.substr(9), nullptr, 16);
                else if (startsWith("AlwaysOn="))       m_AlwaysOn = std::stoi(line.substr(9)) != 0;
                else if (startsWith("StateReset="))     m_StateReset = std::stoi(line.substr(11)) != 0;
                else if (startsWith("PositionNudge="))  m_PositionNudge = std::stof(line.substr(14));
                else if (startsWith("SkipHeightCheck=")) m_SkipHeightCheck = std::stoi(line.substr(15)) != 0;
            } catch (...) {}
        }
    }
    if (m_Mode < 0 || m_Mode > 3) m_Mode = 3;
    PushSettingsToHooks();
}

bool BreakfallForcePlugin::ApplyBytePatches()
{
    if (m_PatchesApplied) return true;

    // Read current bytes and fail-safe if the running build doesn't match the
    // dump-verified layout. NOTE: the epilogue thunk at 0x141A3C293 is the LAST
    // instruction of its block — NOPing it would fall off the end. Never patch it.
    memcpy(m_OrigGate, (void*)ADDR_GATE_VMTHUNK, 5);
    if (m_OrigGate[0] != 0xE9) {
        Log("ABORT: gate bytes are not the VM thunk (build mismatch?). No patch applied.");
        return false;
    }
    if (m_SkipHeightCheck) {
        memcpy(m_OrigHeightJbe, (void*)ADDR_HEIGHT_JBE, 2);
        if (m_OrigHeightJbe[0] != 0x76) {
            Log("ABORT: height-check JBE not found (build mismatch?). No patch applied.");
            return false;
        }
    }

    bool ok = WriteBytes(ADDR_GATE_VMTHUNK, NOP5, 5);
    if (ok && m_SkipHeightCheck)
        ok = WriteBytes(ADDR_HEIGHT_JBE, NOP2, 2);

    if (ok) {
        m_PatchesApplied = true;
        Log(std::string("Byte patches APPLIED (mode 2, alwaysOn=") + (m_AlwaysOn ? "1" : "0")
            + ", skipHeight=" + (m_SkipHeightCheck ? "1" : "0") + ")");
    } else {
        Log("Byte patch FAILED (VirtualProtect?).");
    }
    return ok;
}

void BreakfallForcePlugin::RestoreBytePatches()
{
    if (!m_PatchesApplied) return;
    WriteBytes(ADDR_GATE_VMTHUNK, m_OrigGate, 5);
    if (m_SkipHeightCheck && m_OrigHeightJbe[0] == 0x76)
        WriteBytes(ADDR_HEIGHT_JBE, m_OrigHeightJbe, 2);
    m_PatchesApplied = false;
    Log("Byte patches RESTORED.");
}

void BreakfallForcePlugin::Init()
{
    PushSettingsToHooks();
    Log(std::string("ini=") + GetIniPath() + " log=" + GetLogPath());
    switch (m_Mode) {
    case 1: {
        static AutoAssembleWrapper<GateAssistHook> hookWrapper;
        hookWrapper.Activate();
        m_HookInstalled = true;
        Log("Mode 1 active: assist hooks installed at 0x141A3C0E1 / 0x141A3C293 (stolen VM bytes re-executed).");
        break;
    }
    case 2:
        if (m_AlwaysOn) ApplyBytePatches();
        else Log("Mode 2 armed: patches apply while ForceKey is held.");
        break;
    case 3:
        Log("Mode 3 armed: press the ForceKey to run the game's own freefall enter on the current state node.");
        break;
    default:
        Log("Mode 0: plugin inactive.");
        break;
    }
}

void BreakfallForcePlugin::OnUpdate()
{
    if (m_Mode == 0) return;

    const bool keyDown = IsKeyDown(m_ForceKey);
    s_ForcedActive = (m_Mode != 2) ? keyDown : (m_AlwaysOn || keyDown);

    const bool risingEdge = keyDown && !m_PrevKeyDown;
    m_PrevKeyDown = keyDown;

    if (m_Mode == 2 && !m_AlwaysOn) {
        if (risingEdge) ApplyBytePatches();
        else if (!keyDown && m_PatchesApplied) RestoreBytePatches();
        return;
    }

    if (m_Mode == 3 && risingEdge) {
        ForceFreefallNow();
    }
}

void BreakfallForcePlugin::OnImGuiRender()
{
    if (ImGui::Begin("BreakfallForce")) {
        const char* modeName = m_Mode == 0 ? "0: off"
                             : m_Mode == 1 ? "1: hook-assist (freefall-only nudge)"
                             : m_Mode == 2 ? "2: nop gate freefall-test"
                             : "3: force freefall on keypress";
        ImGui::Text("Mode: %s", modeName);
        ImGui::Text("ForceKey: 0x%02X %s", m_ForceKey, IsKeyDown(m_ForceKey) ? "[held]" : "");
        if (m_Mode == 1) {
            ImGui::Text("Gate hook hits: %ld", s_GateHookHits);
            ImGui::Text("Epilogue hook hits: %ld", s_EpilogueHookHits);
            ImGui::Text("Hook: %s", m_HookInstalled ? "installed" : "NOT installed");
        }
        if (m_Mode == 2) {
            ImGui::Text("Byte patches: %s", m_PatchesApplied ? "applied" : "not applied");
        }
        if (m_Mode == 3) {
            ImGui::Text("Force attempts: %ld", s_ForcedCount);
            ImGui::Text("Last node Enter: 0x%llX", s_LastChosenEnter);
            ImGui::Text("StateReset: %s", m_StateReset ? "on" : "off");
        }
        ImGui::Text("Forcing: %s", s_ForcedActive ? "YES" : "no");
    }
    ImGui::End();
}
