#pragma once

#include <cstdint>
#include <functional>

// Runs a queued callback on the game's simulation thread.
//
// This exists because calling into the game from the Present/render thread
// mutates state the render frame does not expect to change - that is exactly
// the crash ExactPositionTeleportPlugin traced to the atom-graph/state-container
// region. The queue is drained from a mid-function hook inside the game's own
// per-frame update instead.
//
// Hook provenance: ExactPositionTeleportPlugin.cpp, DeferredResetHook.
//   constexpr uintptr_t whenSettingFOVforFrame = 0x141F3FE3B;
//   PresetScript_CCodeInTheMiddle(whenSettingFOVforFrame, 6, ...);
// The same 6-byte site is already used by FreeCameraRotationPlugin,
// PhotoModePlugin and photosavemod, so it is proven safe for this build.
class GameThreadDeferredReset
{
public:
    static GameThreadDeferredReset& Get();

    void Install();

    // Called from inside the game-thread hook. Kept minimal on purpose: no
    // string building and no file I/O here.
    void ServiceOnGameThread();

    bool IsInstalled() const { return m_Installed; }
    void SetExecutor(std::function<void()> executor) { m_Executor = std::move(executor); }

private:
    bool m_Installed = false;
    std::function<void()> m_Executor;
};
