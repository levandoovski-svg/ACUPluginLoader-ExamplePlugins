#pragma once

#include <cstdint>
#include <XInput.h>

/*
 * Split-Screen Local Co-op plugin for Assassin's Creed Unity 1.5.0.
 *
 * Phase 0 (this version): scaffold only.
 *  - INI at Documents\Assassin's Creed Unity\SplitScreenCoop.ini (auto-created).
 *  - Log at the same directory, SplitScreenCoop.log.
 *  - ImGui status window (controller poll, settings).
 *  - NO game hooks, NO patched code, NO native calls. Zero gameplay effect when disabled.
 *
 * Phase plan (see MODLOG/split-screen-coop.md for verified evidence):
 *  1) spawn a second Human entity from the co-op SpawnCharacter/SpawnPlayer behavior nodes,
 *  2) drive it from a controller (per-Human input path),
 *  3) independent camera for player 2,
 *  4) split-screen rendering (two camera passes, side-by-side viewports),
 *  5) HUDs, health separation, polish.
 */
class SplitScreenCoopPlugin
{
public:
    // Game-independent setup: paths, INI (created if missing), log. Never touches the game.
    void OnBeforeActivate();
    // Called from InitStage_WhenCodePatchesAreSafeToApply. Phase 0: only logging. Returns true.
    bool OnWhenCodePatchesAreSafeToApply(class ACUPluginLoaderInterface& pluginLoader);
    // Per-frame, cheap. Phase 0: XInput controller polling only.
    void OnUpdate();
    // ImGui window shown when the loader menu is open.
    void OnImGuiRender();

private:
    // --- settings ---
    bool m_Enabled = true;
    int m_P2ControllerIndex = 0;   // XInput user index (0..3) driving player 2.
    int m_SplitDirection = 1;      // 1 = side-by-side (P1 left, P2 right); 2 = top/bottom. Reserved.

    // --- paths ---
    fs::path m_IniPath;
    fs::path m_LogPath;

    // --- logging ---
    bool m_LogOpen = false;
    void Log(const char* line);
    void LogF(const char* fmt, ...);

    // --- settings IO ---
    void LoadSettings();
    void SaveSettings();
    static fs::path GetDocumentsDir();

    // --- controller polling (dynamic XInput, no static lib dependency) ---
    struct ControllerInfo
    {
        bool connected = false;
        uint32_t lastPacketNumber = 0;
    };
    bool m_XInputAvailable = false;
    uint32_t m_XInputGetStateFailureStreak = 0;
    ControllerInfo m_Controllers[4];
    void PollControllers();
    void RefreshXInputAvailability();

    // --- ImGui state ---
    bool m_ShowStatusWindow = true;
};
