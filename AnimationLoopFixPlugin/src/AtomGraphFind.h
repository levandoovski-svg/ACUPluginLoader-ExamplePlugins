#pragma once

#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include "ACU/AtomAnimComponent.h"
#include "ACU/AtomGraph.h"
#include "ACU/TypeInfo.h"

#include <cstdint>
#include <functional>
#include <vector>

// Only pointers to these are used here, so forward declarations are enough
// and keep this header cheap for the translation units that include it.
class AtomGraph;
class AtomGraphNode;
class AtomGraphStateNode;
class Animation;

namespace AtomGraphQuery
{
    enum class AtomNodeKind
    {
        Unknown,
        Clip,        // AtomAnimationDataNode: carries shared_Animation
        TimeScale,   // AtomTimeScaleNode: speed multiplier
        Root,        // AtomAnimationRootNode: carries IsLoop
    };

    // Type identity comes from the game's own TypeInfo, exactly the way
    // ACUFixes-0.9.3's DumpAnimationGraph.cpp identifies nodes:
    //     graphNode.Unk028_GetTI().typeName
    // A node's vtable slot 0x28 returns its TypeInfo; TypeInfo::typeName
    // (offset 0x18) is a C string naming the class.
    AtomNodeKind ClassifyNode(AtomGraphNode* node);

    // Walks every AtomGraphStateNode reachable from the root state machine,
    // including the nested state machines and the BlendLayers of
    // AtomLayeringStateNode. The traversal follows the exact structure that
    // DumpAnimationGraph.cpp dumps, so it sees the same set of states.
    void ForEachGraphState(AtomGraph* graph, const std::function<void(AtomGraphStateNode*)>& cb);

    // Every animation clip node in the graph, with the state it belongs to.
    struct ClipEntry
    {
        AtomGraphStateNode* state = nullptr;
        AtomGraphNode* node = nullptr;      // AtomAnimationDataNode*
        uint64 handle = 0;
        float length = 0.0f;                // Animation::Length at +0x48
    };
    void CollectClips(AtomGraph* graph, std::vector<ClipEntry>& out);

    // The AtomAnimationRootNode that this clip feeds, plus the optional
    // AtomTimeScaleNode between them, both reached by following
    // AtomInputPort::BindingType 9 (output-port pointer) links.
    AtomGraphNode* FindRootForClip(AtomGraph* graph, AtomGraphNode* clipNode);
    AtomGraphNode* FindTimeScaleForClip(AtomGraph* graph, AtomGraphNode* clipNode);

    // Graph-wide caches of the root (and wrapper) nodes. Collecting these once
    // and then matching by pointer is what keeps per-clip lookups cheap enough
    // to run for every clip in the graph.
    void CollectRootNodes(AtomGraph* graph, std::vector<AtomGraphNode*>& out);
    AtomGraphNode* FindRootForClipIn(AtomGraphNode* clipNode, const std::vector<AtomGraphNode*>& roots);
    AtomGraphNode* FindTimeScaleForClipIn(AtomGraphNode* clipNode, const std::vector<AtomGraphNode*>& roots);

    AtomGraph* GetPlayerAtomGraph();
}
