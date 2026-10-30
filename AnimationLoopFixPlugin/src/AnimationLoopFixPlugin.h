#pragma once

#include <cstdint>
#include <string>
#include <vector>

class AtomGraph;
class AtomGraphNode;

// AnimationLoopFixPlugin
//
// Swaps one animation clip inside the player's AtomGraph by exact managed
// handle, and removes the loop that a shorter replacement produces.
//
// Root cause (verified against ACU 1.5.0 artifacts in this repository):
//   * AtomAnimationDataBaseNode - the class that holds shared_Animation - has
//     no loop and no duration field at all; it only streams the clip.
//   * AtomAnimationRootNode::IsLoop (offset 0x70) is the only loop flag in the
//     node family, and the runtime dump D:\Games\...\AtomGraphDump.txt shows
//     every single one of the graph's 4022 transition conditions is a
//     graph-variable or MarkUp comparison - there is not one
//     PLAYBACK_PERCENTAGE transition anywhere. So the clip restart is purely
//     IsLoop, and the state's exit is driven by the human state machine, whose
//     clock the replacement clip cannot change.
//   => A shorter replacement reaches 100% and IsLoop restarts it, looping
//      until the parkour state is torn down.
//
// So the fix is: swap the clip, and clear IsLoop on the AtomAnimationRootNode
// that the swapped data node feeds. Both are restored when the plugin is
// toggled off. The player's human state is never touched by this path.
class AnimationLoopFixPlugin
{
public:
    void OnBeforeActivate();
    bool OnWhenCodePatchesAreSafeToApply();
    void OnUpdate();
    void OnImGuiRender();

private:
    // --- configuration ---
    void LoadSettings();
    void SaveSettings();
    void EnsureIniFile();
    std::string GetIniPath() const;

    // --- status / log ---
    void SetStatus(const std::string& status);
    void Log(const std::string& line) const;
    std::string GetLogPath() const;

    // --- graph patching ---
    struct AppliedPatch
    {
        uint64_t originalHandle = 0;
        float originalLength = 0.0f;
        AtomGraphNode* clipNode = nullptr;      // AtomAnimationDataNode*
        AtomGraphNode* rootNode = nullptr;      // AtomAnimationRootNode*
        AtomGraphNode* timeScaleNode = nullptr; // AtomTimeScaleNode*
        uint8_t originalIsLoop = 0xFF;          // 0xFF = not read yet
        bool replacementInstalled = false;
    };

    void Rescan();
    void ApplyPatch();
    void RevertPatch();
    void DumpClipTable();

    // --- deferred game-thread reset (optional, off by default) ---
    void QueueDeferredReset(uint64_t delayMs);
    void ServiceDeferredReset();

    // settings
    bool     m_PluginEnabled = false;
    bool     m_PatchIsLoop = true;
    uint64_t m_OriginalHandle = 0;
    uint64_t m_ReplacementHandle = 0;
    bool     m_ResetAuto = false;
    int      m_ResetIntervalMs = 1000;
    int      m_ToggleKey = VK_F10;
    int      m_DumpClipsKey = 'J'; // writes every clip node's handle to the log

    // discovered graph state
    AtomGraph* m_LastGraph = nullptr;   // checked on revert, see RevertPatch()
    std::vector<AppliedPatch> m_Patches;
    int m_ClipCount = 0;

    // deferred reset bookkeeping
    bool     m_DeferredResetRequested = false;
    uint64_t m_DeferredResetAtMs = 0;
    uint64_t m_NextResetAtMs = 0;
    bool     m_DeferredPendingReport = false;
    bool     m_DeferredHookInstalled = false;
    int      m_ResetCount = 0;

    // ui
    bool m_WaitingForKey = false;
    bool m_PrevKeyDown = false;
    bool m_ShowDebug = false;

    std::string m_IniPath;
    std::string m_LastStatus = "Not initialized";
};
