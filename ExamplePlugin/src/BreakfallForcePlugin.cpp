#include "pch.h"
#include "BreakfallForcePlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "vmath/vmath.h"
#include <shlobj.h>

// ── Addresses verified against the runtime dump of the user's ACU.exe ────────
// MODLOG/breakfall-gate.md (sha256 748abdc6456397fe...; no ASLR, base 0x140000000).
// All three sites are exact instruction boundaries in THIS build. The ACUFixes
// 1.5.1 addresses (0x141A3B4D1 / 0x141A3B683) belong to a different game build
// (0x141A3B683 is not even an instruction boundary here) — do not use them.
static constexpr uintptr_t ADDR_GATE_VMTHUNK     = 0x141A3C0E1; // E9 3A 40 3F FE -> 0x13FE30120 (Denuvo VM)
static constexpr uintptr_t ADDR_EPILOGUE_VMTHUNK = 0x141A3C293; // E9 88 3E 3E FE -> 0x13FE20120 (Denuvo VM)
static constexpr uintptr_t ADDR_HEIGHT_JBE       = 0x141A3C21D; // 76 0A : COMISS height-check -> epilogue on <=

// ── Settings mirrored for the AutoAssemblerKinda callbacks (plain functions) ─
static bool     s_ForcedActive   = false;
static float    s_PositionNudge  = 1.0f;
static Vector4f s_SavedFeetPos   {};

static bool IsKeyDown(unsigned char vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ── Mode 1: library-proven assist hooks (ACUFixes-0.9.3 Hack_BreakfallAndLedgeGrab) ──
// Steals the 5-byte VM thunks, runs our callback, then re-executes the stolen
// JMP (isNeedToExecuteStolenBytesAfterwards = true) so the virtualized test
// itself still runs — with our tweaked input.
static void Hook_GateTest_Assist(AllRegisters* params)
{
    Vector4f* feetPos = (Vector4f*)params->rbp_;
    s_SavedFeetPos = *feetPos;
    if (!s_ForcedActive)
        return;
    // The gate scans for a grabbable ledge from the feet position; pretend the
    // player is one nudge closer along the ground-plane movement direction.
    // (Component-wise: this vmath build has no Vector4f+Vector3f operator.)
    Vector3f* moveDir = (Vector3f*)params->r14_;
    feetPos->x += moveDir->x * s_PositionNudge;
    feetPos->y += moveDir->y * s_PositionNudge;
    feetPos->z += moveDir->z * s_PositionNudge;
}

static void Hook_GateEpilogue_Assist(AllRegisters* params)
{
    // Restore before the modified vector feeds the "bump against wall" test,
    // otherwise Arno repeatedly bumps into walls on the way down (library note).
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

// ── Mode 2: reversible byte patches ──────────────────────────────────────────
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

void BreakfallForcePlugin::PushSettingsToHooks()
{
    s_PositionNudge = m_PositionNudge;
}

void BreakfallForcePlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            auto startsWith = [&line](const char* k) { return line.rfind(k, 0) == 0; };
            try {
                if (startsWith("Mode="))             m_Mode = std::stoi(line.substr(5));
                else if (startsWith("ForceKey="))    m_ForceKey = (unsigned char)std::stoi(line.substr(9), nullptr, 16);
                else if (startsWith("AlwaysOn="))    m_AlwaysOn = std::stoi(line.substr(9)) != 0;
                else if (startsWith("PositionNudge=")) m_PositionNudge = std::stof(line.substr(14));
                else if (startsWith("SkipHeightCheck=")) m_SkipHeightCheck = std::stoi(line.substr(15)) != 0;
            } catch (...) {}
        }
    }
    if (m_Mode < 0 || m_Mode > 2) m_Mode = 1;
    PushSettingsToHooks();
}

bool BreakfallForcePlugin::ApplyBytePatches()
{
    if (m_PatchesApplied) return true;

    // Read current bytes and fail-safe if the running build doesn't match the
    // dump-verified layout.
    memcpy(m_OrigGate,     (void*)ADDR_GATE_VMTHUNK,     5);
    memcpy(m_OrigEpilogue, (void*)ADDR_EPILOGUE_VMTHUNK, 5);
    if (m_OrigGate[0] != 0xE9 || m_OrigEpilogue[0] != 0xE9) {
        Log("ABORT: gate/epilogue bytes are not VM thunks (build mismatch?). No patch applied.");
        return false;
    }
    if (m_SkipHeightCheck) {
        memcpy(m_OrigHeightJbe, (void*)ADDR_HEIGHT_JBE, 2);
        if (m_OrigHeightJbe[0] != 0x76) {
            Log("ABORT: height-check JBE not found (build mismatch?). No patch applied.");
            return false;
        }
    }

    bool ok = WriteBytes(ADDR_GATE_VMTHUNK, NOP5, 5)
           && WriteBytes(ADDR_EPILOGUE_VMTHUNK, NOP5, 5);
    if (ok && m_SkipHeightCheck)
        ok = WriteBytes(ADDR_HEIGHT_JBE, NOP2, 2);

    if (ok) {
        m_PatchesApplied = true;
        Log(std::string("Byte patches APPLIED (mode 2, key=") + (char)m_ForceKey
            + ", alwaysOn=" + (m_AlwaysOn ? "1" : "0")
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
    WriteBytes(ADDR_EPILOGUE_VMTHUNK, m_OrigEpilogue, 5);
    if (m_SkipHeightCheck && m_OrigHeightJbe[0] == 0x76)
        WriteBytes(ADDR_HEIGHT_JBE, m_OrigHeightJbe, 2);
    m_PatchesApplied = false;
    Log("Byte patches RESTORED.");
}

void BreakfallForcePlugin::Init()
{
    PushSettingsToHooks();
    if (m_Mode == 1) {
        // Library-proven scaffold: construct + activate once, keep alive.
        static AutoAssembleWrapper<GateAssistHook> hookWrapper;
        hookWrapper.Activate();
        m_HookInstalled = true;
        Log("Mode 1 active: assist hooks installed at 0x141A3C0E1 / 0x141A3C293 (stolen VM bytes re-executed).");
    } else if (m_Mode == 2) {
        if (m_AlwaysOn) {
            ApplyBytePatches();
        } else {
            Log("Mode 2 armed: patches apply while ForceKey is held.");
        }
    } else {
        Log("Mode 0: plugin inactive.");
    }
}

void BreakfallForcePlugin::OnUpdate()
{
    if (m_Mode == 0) return;

    const bool keyDown = IsKeyDown(m_ForceKey);
    s_ForcedActive = (m_Mode == 2) ? (m_AlwaysOn || keyDown) : keyDown;

    if (m_Mode == 2 && !m_AlwaysOn) {
        if (keyDown && !m_PrevKeyDown) ApplyBytePatches();
        else if (!keyDown && m_PrevKeyDown) RestoreBytePatches();
    }
    m_PrevKeyDown = keyDown;
}

void BreakfallForcePlugin::OnImGuiRender()
{
    if (ImGui::Begin("BreakfallForce")) {
        const char* modeName = m_Mode == 0 ? "0: off"
                             : m_Mode == 1 ? "1: hook-assist (library)"
                             : "2: force-nop";
        ImGui::Text("Mode: %s", modeName);
        ImGui::Text("ForceKey: 0x%02X", m_ForceKey);
        ImGui::Text("Key held: %s", IsKeyDown(m_ForceKey) ? "yes" : "no");
        ImGui::Text("Forcing: %s", s_ForcedActive ? "YES" : "no");
        if (m_Mode == 2)
            ImGui::Text("Byte patches: %s", m_PatchesApplied ? "applied" : "not applied");
        if (m_Mode == 1)
            ImGui::Text("Hook: %s", m_HookInstalled ? "installed" : "not installed");
        ImGui::Text("Nudge: %.2f", m_PositionNudge);
        ImGui::Text("SkipHeightCheck: %s", m_SkipHeightCheck ? "on" : "off");
    }
    ImGui::End();
}
