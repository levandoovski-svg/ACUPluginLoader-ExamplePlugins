#include "pch.h"
#include "AnimationLoopFixPlugin.h"
#include "AtomGraphFind.h"
#include "GameThreadDeferredReset.h"

#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include "ACU/AtomAnimComponent.h"
#include "ACU/AtomGraph.h"
#include "ACU/AtomGraphNode.h"
#include "ACU/AtomStateNode.h"
#include "ACU/AtomAnimationDataNode.h"
#include "ACU/AtomAnimationRootNode.h"
#include "ACU/Animation.h"
#include "ACU/Human.h"
#include "ACU/World.h"
#include "ACU/ManagedPtrs/ManagedPtrs.h"
#include "ACU/SharedPtr.h"
#include "ACU_DefineNativeFunction.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"

#include <shlobj.h>
#include <cstdio>

namespace
{
    // Replace one clip's shared_Animation with the clip identified by
    // `newHandle`. This is the same swap ACUFixes' own
    // GraphStateNode_PushAnimationDataNode performs, including the strong
    // refcount increment.
    bool InstallClip(AtomGraphNode* clipNode, uint64_t newHandle, uint64_t* outOldHandle)
    {
        __try
        {
            auto* animNode = reinterpret_cast<AtomAnimationDataNode*>(clipNode);
            if (!animNode->shared_Animation)
            {
                return false;
            }
            if (outOldHandle)
            {
                *outOldHandle = animNode->shared_Animation->handle;
            }
            SharedBlock& block = FindOrMakeSharedBlockByHandleAndIncrementStrongRefcount(newHandle);
            animNode->shared_Animation = &static_cast<SharedPtrNew<Animation>&>(block);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // AtomAnimationRootNode::IsLoop at +0x70 is a uint8 that is only ever 0/1.
    bool ReadIsLoop(AtomGraphNode* root, uint8_t& out)
    {
        __try
        {
            auto* animRoot = reinterpret_cast<AtomAnimationRootNode*>(root);
            if (animRoot->IsLoop > 1)
            {
                return false;
            }
            out = animRoot->IsLoop;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WriteIsLoop(AtomGraphNode* root, uint8_t value)
    {
        __try
        {
            reinterpret_cast<AtomAnimationRootNode*>(root)->IsLoop = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::string Hex64(uint64_t v)
    {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
        return buf;
    }

    uint64_t AnimatorNow()
    {
        __try
        {
            auto* world = ACU::GetWorld();
            if (!world)
            {
                return 0;
            }
            return world->clockInWorldWithSlowmotion.currentTimestamp;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Deferred player-state reset. This is the "same mechanism the exact-teleport
// plugin uses" - it is NOT part of the animation fix and is off by default.
// Human__Dispatch_HSTE_ResetBehavior is the reset ACUFixes' debug
// TeleportPlayer helper uses; it must run on the simulation thread.
// ---------------------------------------------------------------------------
DEFINE_GAME_FUNCTION(Human__Dispatch_HSTE_ResetBehavior, 0x14131A530, char, __fastcall,
    (Human* human));

void AnimationLoopFixPlugin::QueueDeferredReset(uint64_t delayMs)
{
    m_DeferredResetRequested = true;
    m_DeferredResetAtMs = AnimatorNow() + delayMs * 30000ull / 1000ull;
}

void AnimationLoopFixPlugin::ServiceDeferredReset()
{
    if (!m_DeferredResetRequested)
    {
        return;
    }
    if (AnimatorNow() < m_DeferredResetAtMs)
    {
        return;
    }
    m_DeferredResetRequested = false;

    // Runs on the game thread, but still guarded: the reset walks the human
    // state tree and a mid-frame state change is hostile territory.
    __try
    {
        Human* human = Human::GetForPlayer();
        if (human)
        {
            Human__Dispatch_HSTE_ResetBehavior(human);
            ++m_ResetCount;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    m_DeferredPendingReport = true;
}

// ---------------------------------------------------------------------------
void AnimationLoopFixPlugin::OnBeforeActivate()
{
    m_IniPath = GetIniPath();
    EnsureIniFile();
    LoadSettings();
    SetStatus("Disabled. Enter the original clip handle, then toggle.");
}

bool AnimationLoopFixPlugin::OnWhenCodePatchesAreSafeToApply()
{
    GameThreadDeferredReset::Get().Install();
    m_DeferredHookInstalled = GameThreadDeferredReset::Get().IsInstalled();
    if (m_DeferredHookInstalled)
    {
        GameThreadDeferredReset::Get().SetExecutor([this]() { ServiceDeferredReset(); });
    }
    return true;
}

void AnimationLoopFixPlugin::OnUpdate()
{
    if (m_DeferredPendingReport)
    {
        m_DeferredPendingReport = false;
        SetStatus("Deferred HSTE reset executed on the game thread");
    }

    const bool keyDown = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    if (keyDown && !m_PrevKeyDown)
    {
        m_PluginEnabled = !m_PluginEnabled;
        SaveSettings();
        if (m_PluginEnabled)
        {
            ApplyPatch();
        }
        else
        {
            RevertPatch();
        }
    }
    m_PrevKeyDown = keyDown;

    if (!m_PluginEnabled)
    {
        return;
    }

    // Capture mode: write every clip node's handle and length to the log so
    // the two handles can be found without guessing them.
    static bool prevDumpDown = false;
    const bool dumpDown = (GetAsyncKeyState(m_DumpClipsKey) & 0x8000) != 0;
    if (dumpDown && !prevDumpDown)
    {
        DumpClipTable();
    }
    prevDumpDown = dumpDown;

    // Experimental option B. Off by default: automatic end-of-clip detection
    // would need a verified runtime signal that this codebase does not have
    // yet, so the interval is explicit rather than inferred.
    if (m_ResetAuto && !m_Patches.empty())
    {
        const uint64_t now = AnimatorNow();
        if (m_NextResetAtMs == 0)
        {
            m_NextResetAtMs = now + static_cast<uint64_t>(m_ResetIntervalMs) * 30000ull / 1000ull;
        }
        else if (now >= m_NextResetAtMs)
        {
            QueueDeferredReset(50);
            m_NextResetAtMs = now + static_cast<uint64_t>(m_ResetIntervalMs) * 30000ull / 1000ull;
        }
    }
}

// ---------------------------------------------------------------------------
// Graph patching
// ---------------------------------------------------------------------------
void AnimationLoopFixPlugin::Rescan()
{
    m_Patches.clear();
    m_ClipCount = 0;

    AtomGraph* graph = AtomGraphQuery::GetPlayerAtomGraph();
    if (!graph)
    {
        m_LastGraph = nullptr;
        return;
    }
    m_LastGraph = graph;

    std::vector<AtomGraphQuery::ClipEntry> clips;
    AtomGraphQuery::CollectClips(graph, clips);
    m_ClipCount = static_cast<int>(clips.size());
    if (!m_OriginalHandle)
    {
        return;
    }

    for (const AtomGraphQuery::ClipEntry& clip : clips)
    {
        if (clip.handle != m_OriginalHandle)
        {
            continue;
        }
        AppliedPatch patch;
        patch.originalHandle = clip.handle;
        patch.originalLength = clip.length;
        patch.clipNode = clip.node;
        patch.rootNode = AtomGraphQuery::FindRootForClip(graph, clip.node);
        patch.timeScaleNode = AtomGraphQuery::FindTimeScaleForClip(graph, clip.node);
        if (patch.rootNode)
        {
            uint8_t isLoop = 0xFF;
            if (ReadIsLoop(patch.rootNode, isLoop))
            {
                patch.originalIsLoop = isLoop;
            }
        }
        m_Patches.push_back(patch);
    }
}

void AnimationLoopFixPlugin::ApplyPatch()
{
    if (!m_OriginalHandle)
    {
        SetStatus("Enter the original clip handle first (it is printed by the capture mode)");
        return;
    }
    if (!m_ReplacementHandle)
    {
        SetStatus("Enter the replacement clip handle first");
        return;
    }

    Rescan();
    if (m_Patches.empty())
    {
        SetStatus("No clip node found with handle " + Hex64(m_OriginalHandle) +
            " (graph has " + std::to_string(m_ClipCount) + " clip nodes)");
        return;
    }

    bool anyRoot = false;
    int installed = 0;
    for (AppliedPatch& patch : m_Patches)
    {
        if (!patch.replacementInstalled)
        {
            uint64_t old = 0;
            if (InstallClip(patch.clipNode, m_ReplacementHandle, &old))
            {
                patch.replacementInstalled = true;
                ++installed;
            }
        }
        if (patch.rootNode && patch.originalIsLoop != 0xFF && m_PatchIsLoop)
        {
            if (patch.originalIsLoop != 0 && WriteIsLoop(patch.rootNode, 0))
            {
                anyRoot = true;
            }
        }
    }

    char msg[256];
    std::snprintf(msg, sizeof(msg),
        "Patched %d node(s): clip %s -> %s, IsLoop cleared on %s",
        installed, Hex64(m_OriginalHandle).c_str(), Hex64(m_ReplacementHandle).c_str(),
        anyRoot ? "the root node(s)" : "NO root node found");
    SetStatus(msg);
}

void AnimationLoopFixPlugin::RevertPatch()
{
    if (m_Patches.empty())
    {
        return;
    }
    // The AtomGraph object is rebuilt when the player entity is reloaded. If
    // that happened, the cached node pointers are dangling and writing through
    // them would corrupt whatever now owns that memory, so skip the restore
    // rather than guess. The graph file itself is untouched on disk, so the
    // replacement is transient anyway.
    if (AtomGraphQuery::GetPlayerAtomGraph() != m_LastGraph)
    {
        SetStatus("Graph object changed since the patch was applied; skipping the restore.");
        Log("RevertPatch: graph object replaced (" + Hex64(reinterpret_cast<uint64_t>(m_LastGraph)) +
            "), cached node pointers abandoned.");
        m_Patches.clear();
        m_ResetCount = 0;
        return;
    }
    int reverted = 0;
    for (AppliedPatch& patch : m_Patches)
    {
        if (patch.replacementInstalled && patch.originalHandle)
        {
            if (InstallClip(patch.clipNode, patch.originalHandle, nullptr))
            {
                patch.replacementInstalled = false;
                ++reverted;
            }
        }
        if (patch.rootNode && patch.originalIsLoop != 0xFF)
        {
            WriteIsLoop(patch.rootNode, patch.originalIsLoop);
        }
    }
    char msg[160];
    std::snprintf(msg, sizeof(msg), "Reverted %d clip node(s) and restored IsLoop", reverted);
    SetStatus(msg);
    m_Patches.clear();
    m_ResetCount = 0;
}

void AnimationLoopFixPlugin::DumpClipTable()
{
    AtomGraph* graph = AtomGraphQuery::GetPlayerAtomGraph();
    if (!graph)
    {
        SetStatus("Cannot dump: player AtomGraph is unavailable");
        return;
    }
    std::vector<AtomGraphQuery::ClipEntry> clips;
    AtomGraphQuery::CollectClips(graph, clips);
    // The root-node list is collected once and reused for every clip; doing a
    // full graph walk per clip would be quadratic and would stall the frame.
    std::vector<AtomGraphNode*> roots;
    AtomGraphQuery::CollectRootNodes(graph, roots);

    std::ofstream out(GetLogPath(), std::ios::app);
    if (!out)
    {
        SetStatus("Cannot open the log file for the dump");
        return;
    }
    out << "---- clip dump (" << clips.size() << " clip nodes, "
        << roots.size() << " root nodes) ----\n";
    for (size_t i = 0; i < clips.size(); ++i)
    {
        const AtomGraphQuery::ClipEntry& clip = clips[i];
        AtomGraphNode* root = AtomGraphQuery::FindRootForClipIn(clip.node, roots);
        AtomGraphNode* ts = AtomGraphQuery::FindTimeScaleForClipIn(clip.node, roots);
        // RuntimeStateID lets this line be matched back to AtomGraphDump.txt.
        uint32_t runtimeStateID = clip.state ? clip.state->RuntimeStateID : 0;
        uint8_t isLoop = 0xFF;
        if (root)
        {
            ReadIsLoop(root, isLoop);
        }
        out << "clip #" << i
            << " state=" << runtimeStateID
            << " handle=" << Hex64(clip.handle)
            << " length=" << clip.length
            << " root=" << (root ? "yes" : "NO")
            << " isLoop=" << (isLoop == 0xFF ? -1 : static_cast<int>(isLoop))
            << " timescale=" << (ts ? "yes" : "no") << "\n";
    }
    out.flush();
    SetStatus("Dumped " + std::to_string(clips.size()) + " clip nodes to the log");
}

// ---------------------------------------------------------------------------
// Settings and logging
// ---------------------------------------------------------------------------
std::string AnimationLoopFixPlugin::GetIniPath() const
{
    char docs[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
    {
        return std::string(docs) + "\\Assassin's Creed Unity\\AnimationLoopFixPlugin.ini";
    }
    return "AnimationLoopFixPlugin.ini";
}

std::string AnimationLoopFixPlugin::GetLogPath() const
{
    char docs[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
    {
        return std::string(docs) + "\\Assassin's Creed Unity\\AnimationLoopFixPlugin.log";
    }
    return "AnimationLoopFixPlugin.log";
}

void AnimationLoopFixPlugin::EnsureIniFile()
{
    if (fs::exists(m_IniPath))
    {
        return;
    }
    std::ofstream f(m_IniPath, std::ios::trunc);
    if (!f)
    {
        return;
    }
    f << "// AnimationLoopFixPlugin settings\n"
      << "// Handles are 64-bit managed-object handles, e.g. 32302899222.\n"
      << "// Press DumpClipsKey in game to write every clip node's handle to the log.\n"
      << "PluginEnabled=0\n"
      << "PatchIsLoop=1\n"
      << "OriginalHandle=0\n"
      << "ReplacementHandle=0\n"
      << "ResetAuto=0\n"
      << "ResetIntervalMs=1000\n"
      << "ToggleKey=79\n"
      << "DumpClipsKey=74\n";
}

void AnimationLoopFixPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f)
    {
        return;
    }
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("PluginEnabled=", 0) == 0)
        {
            m_PluginEnabled = std::stoi(line.substr(14)) != 0;
        }
        else if (line.rfind("PatchIsLoop=", 0) == 0)
        {
            m_PatchIsLoop = std::stoi(line.substr(12)) != 0;
        }
        else if (line.rfind("OriginalHandle=", 0) == 0)
        {
            m_OriginalHandle = std::stoull(line.substr(15));
        }
        else if (line.rfind("ReplacementHandle=", 0) == 0)
        {
            m_ReplacementHandle = std::stoull(line.substr(17));
        }
        else if (line.rfind("ResetAuto=", 0) == 0)
        {
            m_ResetAuto = std::stoi(line.substr(10)) != 0;
        }
        else if (line.rfind("ResetIntervalMs=", 0) == 0)
        {
            m_ResetIntervalMs = std::stoi(line.substr(16));
        }
        else if (line.rfind("ToggleKey=", 0) == 0)
        {
            m_ToggleKey = std::stoi(line.substr(10), nullptr, 10);
        }
        else if (line.rfind("DumpClipsKey=", 0) == 0)
        {
            m_DumpClipsKey = std::stoi(line.substr(13), nullptr, 10);
        }
    }
}

void AnimationLoopFixPlugin::SaveSettings()
{
    std::ofstream f(GetIniPath(), std::ios::trunc);
    if (!f)
    {
        return;
    }
    f << "PluginEnabled=" << (m_PluginEnabled ? 1 : 0) << "\n"
      << "PatchIsLoop=" << (m_PatchIsLoop ? 1 : 0) << "\n"
      << "OriginalHandle=" << m_OriginalHandle << "\n"
      << "ReplacementHandle=" << m_ReplacementHandle << "\n"
      << "ResetAuto=" << (m_ResetAuto ? 1 : 0) << "\n"
      << "ResetIntervalMs=" << m_ResetIntervalMs << "\n"
      << "ToggleKey=" << m_ToggleKey << "\n"
      << "DumpClipsKey=" << m_DumpClipsKey << "\n";
}

void AnimationLoopFixPlugin::SetStatus(const std::string& status)
{
    m_LastStatus = status;
}

void AnimationLoopFixPlugin::Log(const std::string& line) const
{
    std::ofstream out(GetLogPath(), std::ios::app);
    if (out)
    {
        out << line << "\n";
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
void AnimationLoopFixPlugin::OnImGuiRender()
{
    if (ImGui::Checkbox("Enable animation loop fix", &m_PluginEnabled))
    {
        SaveSettings();
        if (m_PluginEnabled)
        {
            ApplyPatch();
        }
        else
        {
            RevertPatch();
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Swaps the clip and clears IsLoop on the affected root node.");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(toggle key: 0x%02X)", m_ToggleKey);

    if (m_ShowDebug && ImGui::TreeNode("Clip handles"))
    {
        ImGui::InputScalar("Original handle", ImGuiDataType_U64, &m_OriginalHandle);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The clip that is currently in the graph (the long one).\n"
                              "Use Dump clips to read it from the log.");
        }
        ImGui::InputScalar("Replacement handle", ImGuiDataType_U64, &m_ReplacementHandle);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The clip to play instead (the short one).");
        }
        if (ImGui::Button("Dump clips to log"))
        {
            DumpClipTable();
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply"))
        {
            ApplyPatch();
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
        {
            RevertPatch();
        }
        ImGui::Checkbox("Clear IsLoop on the affected root node", &m_PatchIsLoop);
        ImGui::TreePop();
    }

    if (m_ShowDebug && ImGui::TreeNode("Deferred player-state reset (experimental)"))
    {
        ImGui::Checkbox("Dispatch HSTE reset on an interval", &m_ResetAuto);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("OFF by default. This is the exact-teleport plugin's reset,\n"
                              "dispatched on the game thread. Automatic end-of-clip detection\n"
                              "needs a verified runtime signal; until then the interval is explicit.");
        }
        ImGui::SliderInt("Interval (ms)", &m_ResetIntervalMs, 100, 5000);
        ImGui::Text("Resets dispatched this session: %d", m_ResetCount);
        ImGui::TreePop();
    }

    ImGui::Checkbox("Debug", &m_ShowDebug);

    ImGui::Separator();
    ImGui::TextWrapped("Status: %s", m_LastStatus.c_str());
}
