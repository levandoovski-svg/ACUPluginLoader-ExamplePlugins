#include "pch.h"
#include "ExactPositionTeleportPlugin.h"

#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include "ACU/Human.h"
#include "ACU_DefineNativeFunction.h"
#include "ParkourDebugging/EnumParkourAction.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <shlobj.h>

namespace
{
    constexpr int kDefaultForceKey = 'T'; // VK_T
    constexpr int kDefaultBreakfallDelayMs = 100;

    // ACUFixes-0.9.3 addresses for the build represented by the runtime dump
    // in MODLOG/breakfall-gate.md (base 0x140000000, no ASLR).
    //
    // BaseEntity::SetTransform(Entity* this, const Matrix4f* newTransform)
    // copies the supplied matrix into the entity's main transform. The
    // ACUFixes hook confirms the RCX/RDX calling convention.
    DEFINE_GAME_FUNCTION(BaseEntity__SetTransform, 0x1425DE360, void, __fastcall,
        (Entity* entity, const Matrix4f* newTransform));

    // This is the reset call used by ACUFixes-0.9.3's debug TeleportPlayer
    // helper after it writes the player's position.
    DEFINE_GAME_FUNCTION(Human__Dispatch_HSTE_ResetBehavior, 0x14131A530, char, __fastcall,
        (Human* human));

    // Parkour action REQUEST pipeline. The entry thunk clears the flag
    // register and jumps to the real request implementation. Calling with
    // EnumParkourAction::breakfall (18) asks the game to execute the normal
    // breakfall action rather than redirecting a state Enter directly.
    DEFINE_GAME_FUNCTION(ParkourAction__Request, 0x140C85280, void, __fastcall,
        (int actionId, int actionFlags));

    bool BytesMatch(uintptr_t address, const unsigned char* expected, size_t count)
    {
        __try
        {
            return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    Entity* GetPlayerSafely()
    {
        __try
        {
            return ACU::GetPlayer();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    Human* GetPlayerHumanSafely()
    {
        __try
        {
            return Human::GetForPlayer();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ApplyNativeSetTransform(Entity* entity, const Matrix4f* transform)
    {
        __try
        {
            BaseEntity__SetTransform(entity, transform);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool DispatchHumanReset(Human* human)
    {
        __try
        {
            Human__Dispatch_HSTE_ResetBehavior(human);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool RequestBreakfallActionSafely()
    {
        __try
        {
            ParkourAction__Request(static_cast<int>(EnumParkourAction::breakfall), 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::string GetDocumentsIniPath()
    {
        char documents[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, documents)))
        {
            return std::string(documents) + "\\Assassin's Creed Unity\\ExactPositionTeleport.ini";
        }
        return "ExactPositionTeleport.ini";
    }

    bool WriteDefaultIni(const std::filesystem::path& path)
    {
        std::ofstream file(path, std::ios::trunc);
        if (!file)
        {
            return false;
        }

        file << "// ExactPositionTeleport settings\n"
             << "// ForceKey is a Windows virtual-key code in hexadecimal (54 = T).\n"
             << "ForceKey=" << std::hex << kDefaultForceKey << std::dec << "\n"
             << "// Request EnumParkourAction::breakfall after the exact-position teleport.\n"
             << "BreakfallAfterTeleport=1\n"
             << "// Delay in milliseconds between the teleport/reset and the breakfall request.\n"
             << "BreakfallDelayMs=" << kDefaultBreakfallDelayMs << "\n";
        file.flush();
        return static_cast<bool>(file);
    }
}

void ExactPositionTeleportPlugin::Init()
{
    // Prefer the directory containing ACU.exe. If that directory is not
    // writable (for example, a protected Program Files install), fall back to
    // the standard writable Assassin's Creed Unity documents directory.
    m_IniPath = ResolvePrimaryIniPath();
    const bool iniReady = EnsureIniFile();
    LoadSettings();

    // Fail closed if this DLL is loaded into another ACU build. These are
    // exact signatures observed in the user's runtime dump, not guessed.
    static const unsigned char expectedSetTransform[] = {
        0x0F, 0x28, 0x02, 0x0F, 0x29, 0x41, 0x20, 0x0F
    };
    static const unsigned char expectedResetBehavior[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20
    };
    static const unsigned char expectedParkourRequest[] = {
        0x33, 0xD2, 0xE9, 0x09, 0x00, 0x00, 0x00
    };

    const bool setTransformMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(BaseEntity__SetTransform),
        expectedSetTransform,
        sizeof(expectedSetTransform));
    const bool resetBehaviorMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(Human__Dispatch_HSTE_ResetBehavior),
        expectedResetBehavior,
        sizeof(expectedResetBehavior));
    const bool parkourRequestMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(ParkourAction__Request),
        expectedParkourRequest,
        sizeof(expectedParkourRequest));

    m_BuildCompatible = setTransformMatches && resetBehaviorMatches;
    m_BreakfallRequestCompatible = parkourRequestMatches;
    m_PrevKeyDown = (GetAsyncKeyState(m_ForceKey) & 0x8000) != 0;

    Log("ini=" + m_IniPath + " log=" + GetLogPath());

    if (!iniReady)
    {
        SetStatus("WARNING: could not create the settings INI file");
    }
    else if (!m_BuildCompatible)
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "ABORT: build signature mismatch (SetTransform=%s, ResetBehavior=%s)",
            setTransformMatches ? "ok" : "bad",
            resetBehaviorMatches ? "ok" : "bad");
        SetStatus(details);
    }
    else
    {
        char details[224];
        std::snprintf(details, sizeof(details),
            "Ready - force key 0x%02X; breakfall after teleport %s (request %s)",
            m_ForceKey,
            m_BreakfallAfterTeleport ? "on" : "off",
            m_BreakfallRequestCompatible ? "compatible" : "UNAVAILABLE");
        SetStatus(details);
    }
}

void ExactPositionTeleportPlugin::OnUpdate()
{
    const bool keyDown = (GetAsyncKeyState(m_ForceKey) & 0x8000) != 0;
    if (!m_WaitingForKey && keyDown && !m_PrevKeyDown)
    {
        TriggerForceFreefall();
    }
    m_PrevKeyDown = keyDown;

    ProcessPendingBreakfall();
}

void ExactPositionTeleportPlugin::TriggerForceFreefall()
{
    if (!TeleportToExactCurrentPosition())
    {
        m_PendingBreakfall = false;
        return;
    }

    if (!m_BreakfallAfterTeleport)
    {
        m_PendingBreakfall = false;
        return;
    }

    if (!m_BreakfallRequestCompatible)
    {
        m_PendingBreakfall = false;
        m_LastBreakfallRequested = false;
        SetStatus(m_LastStatus + "; breakfall request unavailable for this build");
        return;
    }

    // Give the reset/freefall transition a few frames to establish before
    // asking the parkour pipeline to execute breakfall.
    m_PendingBreakfall = true;
    m_LastBreakfallRequested = false;
    m_BreakfallRequestAt = GetTickCount64() + static_cast<uint64_t>(m_BreakfallDelayMs);
    SetStatus(m_LastStatus + "; breakfall queued");
}

bool ExactPositionTeleportPlugin::TeleportToExactCurrentPosition()
{
    m_LastTransformApplied = false;
    m_LastResetDispatched = false;

    if (!m_BuildCompatible)
    {
        SetStatus("Teleport skipped: incompatible ACU build");
        return false;
    }

    Entity* player = GetPlayerSafely();
    if (!player)
    {
        SetStatus("Teleport skipped: player entity is unavailable");
        return false;
    }

    // Snapshot before writing. The destination is exactly the position that
    // was sampled; no offset or movement is introduced.
    const Vector3f exactPosition = player->GetPosition();
    const Matrix4f exactTransform = player->GetTransform();

    m_LastX = exactPosition.x;
    m_LastY = exactPosition.y;
    m_LastZ = exactPosition.z;

    // Keep the library-proven position write, then also send the unchanged
    // matrix through the native transform setter so this is a real transform
    // update rather than merely a same-value assignment that the compiler
    // could discard.
    player->GetPosition() = exactPosition;
    m_LastTransformApplied = ApplyNativeSetTransform(player, &exactTransform);

    if (!m_LastTransformApplied)
    {
        SetStatus("Teleport failed: native SetTransform raised an exception");
        return false;
    }

    Human* human = GetPlayerHumanSafely();
    if (!human)
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "Transform applied at (%.3f, %.3f, %.3f), but player Human was unavailable",
            m_LastX, m_LastY, m_LastZ);
        SetStatus(details);
        return false;
    }

    // This is the same HSTE reset used by ACUFixes' debug TeleportPlayer.
    // It lets the game cancel/re-evaluate the current parkour behavior after
    // seeing the transformed position.
    m_LastResetDispatched = DispatchHumanReset(human);
    if (!m_LastResetDispatched)
    {
        SetStatus("Teleport failed: HSTE ResetBehavior raised an exception");
        return false;
    }

    char details[224];
    std::snprintf(details, sizeof(details),
        "Teleported to exact position (%.3f, %.3f, %.3f); reset dispatched",
        m_LastX, m_LastY, m_LastZ);
    SetStatus(details);
    return true;
}

void ExactPositionTeleportPlugin::ProcessPendingBreakfall()
{
    if (!m_PendingBreakfall || GetTickCount64() < m_BreakfallRequestAt)
    {
        return;
    }

    m_PendingBreakfall = false;
    if (!m_BreakfallRequestCompatible)
    {
        m_LastBreakfallRequested = false;
        SetStatus("Breakfall request skipped: incompatible ACU build");
        return;
    }

    m_LastBreakfallRequested = RequestBreakfallActionSafely();
    SetStatus(m_LastBreakfallRequested
        ? "Breakfall action 18 requested through the parkour-action pipeline"
        : "Breakfall action request raised an exception");
}

void ExactPositionTeleportPlugin::OnImGuiRender()
{
    ImGui::Begin("Exact Position Teleport");

    ImGui::Text("Force-freefall / teleport key: 0x%02X", m_ForceKey);
    if (m_ForceKey >= 0x20 && m_ForceKey <= 0x7E)
    {
        ImGui::SameLine();
        ImGui::Text("('%c')", static_cast<char>(m_ForceKey));
    }
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey ? "Cancel rebind" : "Rebind"))
    {
        m_WaitingForKey = !m_WaitingForKey;
        if (m_WaitingForKey)
        {
            for (int vk = 1; vk <= 0xFF; ++vk)
            {
                m_CaptureKeyWasDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
            }
        }
    }

    if (m_WaitingForKey)
    {
        // Capture the first fresh key/mouse-button transition. This runs
        // after the Cancel button is processed, so clicking Cancel cannot
        // accidentally bind the mouse button.
        for (int vk = 1; vk <= 0xFF; ++vk)
        {
            const bool keyDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool newlyPressed = keyDown && !m_CaptureKeyWasDown[vk];
            m_CaptureKeyWasDown[vk] = keyDown;
            if (!newlyPressed)
            {
                continue;
            }

            m_ForceKey = vk;
            m_WaitingForKey = false;
            // The newly captured key may still be held. Requiring a fresh
            // press prevents an accidental immediate teleport.
            m_PrevKeyDown = keyDown;
            SaveSettings();
            SetStatus("Force key rebound to 0x" + [&] {
                char keyText[8];
                std::snprintf(keyText, sizeof(keyText), "%02X", vk);
                return std::string(keyText);
            }());
            break;
        }

        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f),
            "Press any keyboard key or mouse button to bind it...");
    }

    bool breakfallAfterTeleport = m_BreakfallAfterTeleport;
    if (ImGui::Checkbox("Execute Breakfall after teleport", &breakfallAfterTeleport))
    {
        m_BreakfallAfterTeleport = breakfallAfterTeleport;
        if (!m_BreakfallAfterTeleport)
        {
            m_PendingBreakfall = false;
        }
        SaveSettings();
    }

    int breakfallDelayMs = m_BreakfallDelayMs;
    if (ImGui::InputInt("Breakfall request delay (ms)", &breakfallDelayMs))
    {
        if (breakfallDelayMs < 0) breakfallDelayMs = 0;
        if (breakfallDelayMs > 2000) breakfallDelayMs = 2000;
        m_BreakfallDelayMs = breakfallDelayMs;
        SaveSettings();
    }

    ImGui::Separator();
    ImGui::Text("Build signature: %s", m_BuildCompatible ? "compatible" : "MISMATCH");
    ImGui::Text("Breakfall request pipeline: %s",
        m_BreakfallRequestCompatible ? "compatible" : "UNAVAILABLE");
    ImGui::Text("Last transform applied: %s", m_LastTransformApplied ? "yes" : "no");
    ImGui::Text("Last HSTE reset dispatched: %s", m_LastResetDispatched ? "yes" : "no");
    ImGui::Text("Last breakfall request: %s",
        m_LastBreakfallRequested ? "dispatched" : (m_PendingBreakfall ? "pending" : "no"));
    ImGui::Text("Last position: %.3f, %.3f, %.3f", m_LastX, m_LastY, m_LastZ);
    ImGui::TextWrapped("Settings file: %s", m_IniPath.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("Last result: %s", m_LastStatus.c_str());

    if (ImGui::Button(m_BreakfallAfterTeleport
        ? "Force freefall + Breakfall now"
        : "Force freefall now"))
    {
        TriggerForceFreefall();
    }

    ImGui::End();
}

void ExactPositionTeleportPlugin::LoadSettings()
{
    std::ifstream file(m_IniPath);
    if (!file)
    {
        return;
    }

    std::string line;
    while (std::getline(file, line))
    {
        auto startsWith = [&line](const char* key) { return line.rfind(key, 0) == 0; };
        try
        {
            if (startsWith("ForceKey="))
            {
                const int key = std::stoi(line.substr(9), nullptr, 16);
                if (key >= 1 && key <= 0xFF)
                {
                    m_ForceKey = key;
                }
            }
            else if (startsWith("BreakfallAfterTeleport="))
            {
                m_BreakfallAfterTeleport = std::stoi(line.substr(23)) != 0;
            }
            else if (startsWith("BreakfallDelayMs="))
            {
                const int delay = std::stoi(line.substr(17));
                if (delay >= 0 && delay <= 2000)
                {
                    m_BreakfallDelayMs = delay;
                }
            }
        }
        catch (...)
        {
            // Keep the current/default value for a malformed setting.
        }
    }
}

void ExactPositionTeleportPlugin::SaveSettings()
{
    try
    {
        const std::filesystem::path path(m_IniPath);
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream file(path, std::ios::trunc);
        if (!file)
        {
            return;
        }

        file << "// ExactPositionTeleport settings\n"
             << "// ForceKey is a Windows virtual-key code in hexadecimal (54 = T).\n"
             << "ForceKey=" << std::hex << m_ForceKey << std::dec << "\n"
             << "// Request EnumParkourAction::breakfall after the exact-position teleport.\n"
             << "BreakfallAfterTeleport=" << (m_BreakfallAfterTeleport ? 1 : 0) << "\n"
             << "// Delay in milliseconds between the teleport/reset and the breakfall request.\n"
             << "BreakfallDelayMs=" << m_BreakfallDelayMs << "\n";
    }
    catch (...)
    {
    }
}

bool ExactPositionTeleportPlugin::EnsureIniFile()
{
    auto tryUsePath = [this](const std::string& path) -> bool
    {
        try
        {
            const std::filesystem::path fsPath(path);
            if (!fsPath.parent_path().empty())
            {
                std::filesystem::create_directories(fsPath.parent_path());
            }

            std::error_code fileError;
            const bool exists = std::filesystem::exists(fsPath, fileError);
            if (!exists || (!fileError && std::filesystem::file_size(fsPath, fileError) == 0))
            {
                if (!WriteDefaultIni(fsPath))
                {
                    return false;
                }
            }
            else
            {
                // Confirm that the existing file is writable without erasing
                // the user's settings.
                std::ofstream probe(fsPath, std::ios::app);
                if (!probe)
                {
                    return false;
                }
            }

            m_IniPath = path;
            return true;
        }
        catch (...)
        {
            return false;
        }
    };

    if (tryUsePath(ResolvePrimaryIniPath()))
    {
        return true;
    }

    // A protected game-install directory is a common reason for the primary
    // path to fail. Use the standard ACU writable documents directory then.
    if (tryUsePath(GetDocumentsIniPath()))
    {
        return true;
    }

    m_IniPath = ResolvePrimaryIniPath();
    return false;
}

std::string ExactPositionTeleportPlugin::ResolvePrimaryIniPath() const
{
    char executablePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        const std::filesystem::path path(executablePath);
        return (path.parent_path() / "ExactPositionTeleport.ini").string();
    }

    return GetDocumentsIniPath();
}

void ExactPositionTeleportPlugin::SetStatus(const std::string& status)
{
    m_LastStatus = status;
    Log(status);
}

void ExactPositionTeleportPlugin::Log(const std::string& line) const
{
    const std::string text = "[ExactPositionTeleport] " + line + "\n";
    OutputDebugStringA(text.c_str());

    try
    {
        const std::filesystem::path path(GetLogPath());
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream file(path, std::ios::app);
        if (file)
        {
            file << text;
        }
    }
    catch (...)
    {
    }
}

std::string ExactPositionTeleportPlugin::GetLogPath() const
{
    char documents[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, documents)))
    {
        return std::string(documents) + "\\Assassin's Creed Unity\\ExactPositionTeleport.log";
    }
    return "ExactPositionTeleport.log";
}
