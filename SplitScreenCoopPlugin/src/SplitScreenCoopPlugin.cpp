#include "pch.h"
#include "SplitScreenCoopPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>

// ============================================================================
// Paths
// ============================================================================

fs::path SplitScreenCoopPlugin::GetDocumentsDir()
{
    fs::path result;
    PWSTR knownPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &knownPath)))
    {
        result = knownPath;
    }
    if (knownPath)
    {
        CoTaskMemFree(knownPath);
    }
    return result;
}

// ============================================================================
// Logging
// ============================================================================

void SplitScreenCoopPlugin::Log(const char* line)
{
    if (!m_LogOpen || m_LogPath.empty())
    {
        return;
    }
    std::ofstream log(m_LogPath, std::ios::app);
    if (!log)
    {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm nowTm{};
    localtime_s(&nowTm, &nowTime);
    char timeBuf[32]{};
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &nowTm);
    log << '[' << timeBuf << "] " << line << '\n';
}

void SplitScreenCoopPlugin::LogF(const char* fmt, ...)
{
    char buffer[512]{};
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    Log(buffer);
}

// ============================================================================
// Settings
// ============================================================================

void SplitScreenCoopPlugin::LoadSettings()
{
    m_IniPath = GetDocumentsDir() / L"Assassin's Creed Unity" / L"SplitScreenCoop.ini";

    bool fileExisted = fs::exists(m_IniPath);
    if (!fileExisted)
    {
        SaveSettings(); // Creates folder + file with commented defaults.
        return;
    }

    std::ifstream ini(m_IniPath);
    if (!ini)
    {
        return;
    }
    std::string line;
    while (std::getline(ini, line))
    {
        const auto hashPos = line.find("//");
        if (hashPos != std::string::npos)
        {
            line = line.substr(0, hashPos);
        }
        const auto eqPos = line.find('=');
        if (eqPos == std::string::npos)
        {
            continue;
        }
        const std::string key = line.substr(0, eqPos);
        const std::string value = line.substr(eqPos + 1);
        try
        {
            if (key == "Enabled")
            {
                m_Enabled = std::stoi(value) != 0;
            }
            else if (key == "P2ControllerIndex")
            {
                m_P2ControllerIndex = std::clamp(std::stoi(value), 0, 3);
            }
            else if (key == "SplitDirection")
            {
                m_SplitDirection = (std::stoi(value) == 2) ? 2 : 1;
            }
        }
        catch (...)
        {
            // Malformed value: keep the default.
        }
    }
}

void SplitScreenCoopPlugin::SaveSettings()
{
    if (m_IniPath.empty())
    {
        m_IniPath = GetDocumentsDir() / L"Assassin's Creed Unity" / L"SplitScreenCoop.ini";
    }
    std::error_code ec;
    fs::create_directories(m_IniPath.parent_path(), ec);

    std::ofstream ini(m_IniPath, std::ios::trunc);
    if (!ini)
    {
        return;
    }
    ini << "// SplitScreenCoopPlugin configuration\n"
        << "// Recreated with defaults if this file is missing or corrupt.\n"
        << "// Enabled: master toggle. When 0 the plugin does nothing gameplay-related.\n"
        << "// P2ControllerIndex: XInput controller (0..3) that drives player 2.\n"
        << "// SplitDirection: 1 = side-by-side (P1 left, P2 right), 2 = top/bottom.\n"
        << "Enabled=" << (m_Enabled ? 1 : 0) << "\n"
        << "P2ControllerIndex=" << m_P2ControllerIndex << "\n"
        << "SplitDirection=" << m_SplitDirection << "\n";
}

// ============================================================================
// XInput (loaded dynamically; no static xinput.lib dependency)
// ============================================================================

using PFN_XInputGetState = DWORD(WINAPI*)(DWORD dwUserIndex, XINPUT_STATE* pState);
static PFN_XInputGetState g_XInputGetState = nullptr;

void SplitScreenCoopPlugin::RefreshXInputAvailability()
{
    if (g_XInputGetState)
    {
        return;
    }
    // Prefer the Win8+ DLL, then fall back to older variants.
    for (const char* dllName : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
    {
        if (HMODULE module = LoadLibraryA(dllName))
        {
            g_XInputGetState = (PFN_XInputGetState)GetProcAddress(module, "XInputGetState");
            if (g_XInputGetState)
            {
                m_XInputAvailable = true;
                LogF("XInput loaded from %s", dllName);
                return;
            }
            FreeLibrary(module);
        }
    }
    Log("No XInput DLL could be loaded; controller input unavailable.");
}

void SplitScreenCoopPlugin::PollControllers()
{
    if (!m_Enabled || !g_XInputGetState)
    {
        return;
    }
    for (DWORD index = 0; index < 4; ++index)
    {
        XINPUT_STATE state{};
        const DWORD err = g_XInputGetState(index, &state);
        const bool connected = (err == ERROR_SUCCESS);
        ControllerInfo& info = m_Controllers[index];
        if (connected != info.connected)
        {
            info.connected = connected;
            LogF("Controller %lu %s", (unsigned long)index, connected ? "connected" : "disconnected");
        }
        if (connected)
        {
            info.lastPacketNumber = state.dwPacketNumber;
        }
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void SplitScreenCoopPlugin::OnBeforeActivate()
{
    // Game-independent only (loader contract for this stage): paths, INI, log.
    LoadSettings();
    m_LogPath = m_IniPath.parent_path() / L"SplitScreenCoop.log";
    m_LogOpen = true;
    Log("=== SplitScreenCoopPlugin session start (Phase 0 scaffold) ===");
    LogF("Ini path: %ls", m_IniPath.c_str());
    LogF("Log path: %ls", m_LogPath.c_str());
    LogF("Settings: Enabled=%d P2ControllerIndex=%d SplitDirection=%d",
         m_Enabled ? 1 : 0, m_P2ControllerIndex, m_SplitDirection);
    RefreshXInputAvailability();
}

bool SplitScreenCoopPlugin::OnWhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader)
{
    // Phase 0 applies no code patches and makes no native calls.
    // Log the loader interface version so later phases can cite the environment.
    LogF("Code-patch stage reached. PluginLoader reports itself as compatible. (No patches installed by Phase 0.)");
    (void)pluginLoader;
    return true;
}

// ============================================================================
// Per-frame
// ============================================================================

void SplitScreenCoopPlugin::OnUpdate()
{
    PollControllers();
}

// ============================================================================
// ImGui
// ============================================================================

void SplitScreenCoopPlugin::OnImGuiRender()
{
    if (!m_ShowStatusWindow)
    {
        return;
    }
    if (!ImGui::Begin("Split-Screen Co-op", &m_ShowStatusWindow))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Phase 0 scaffold - no gameplay hooks yet");
    ImGui::Separator();

    if (ImGui::Checkbox("Enabled", &m_Enabled))
    {
        SaveSettings();
        LogF("Enabled set to %d", m_Enabled ? 1 : 0);
    }

    if (ImGui::SliderInt("P2 controller index", &m_P2ControllerIndex, 0, 3))
    {
        m_P2ControllerIndex = std::clamp(m_P2ControllerIndex, 0, 3);
        SaveSettings();
    }
    static const char* splitNames[] = {"Side-by-side (P1 left)", "Top/bottom"};
    int splitIdx = m_SplitDirection - 1;
    if (ImGui::Combo("Split direction (reserved)", &splitIdx, splitNames, 2))
    {
        m_SplitDirection = splitIdx + 1;
        SaveSettings();
    }

    ImGui::SeparatorText("Controllers");
    for (int i = 0; i < 4; ++i)
    {
        const bool isP2 = (i == m_P2ControllerIndex);
        ImGui::Text("Controller %d: %s%s", i,
                    m_Controllers[i].connected ? "connected" : "not connected",
                    isP2 ? "  <- player 2" : "");
    }
    if (!m_XInputAvailable)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "XInput could not be loaded.");
    }

    ImGui::SeparatorText("Paths");
    ImGui::TextDisabled("%ls", m_IniPath.c_str());
    ImGui::TextDisabled("%ls", m_LogPath.c_str());

    ImGui::Separator();
    if (ImGui::Button("Unload this plugin"))
    {
        Log("Unload requested from ImGui.");
        RequestUnloadThisPlugin();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Phase plan: 1 spawn, 2 input, 3 camera, 4 split render, 5 polish");

    ImGui::End();
}
