#include "pch.h"
#include "AtomGraphFind.h"
#include "ACU/AtomGraphStateNode.h"
#include "ACU/AtomStateMachineNode.h"
#include "ACU/AtomLayeringStateNode.h"
#include "ACU/AtomAnimationDataNode.h"
#include "ACU/AtomAnimationRootNode.h"
// NOTE: there is no ACU/AtomTimeScaleNode.h in ACU-RE 0.9.3. The timescale
// node is identified purely by its TypeInfo name ("AtomTimeScaleNode") and
// none of its own fields are read here, so no layout is required.
#include "ACU/AtomGraphNode.h"
#include "ACU/ManagedObject.h"
#include "ACU/ManagedPtrs/ManagedPtrs.h"
#include "ACU/SharedPtr.h"
#include <set>
#include <vector>

namespace
{
    // The game's memory is read through pointers whose layout we only trust
    // after the TypeInfo check, but a wrong pointer can still fault, so all
    // dereferences are wrapped in SEH.
    template <typename T>
    bool SafeRead(uintptr_t addr, T& out)
    {
        __try
        {
            out = *reinterpret_cast<T*>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char* TypeNameOf(void* obj)
    {
        if (!obj)
        {
            return nullptr;
        }
        __try
        {
            // Object::Unk028_GetTI() is declared virtual in ACU/ManagedObject.h
            // and returns the object's TypeInfo. TypeInfo::typeName (offset
            // 0x18) is a C string naming the class. This is exactly how
            // ACUFixes-0.9.3's DumpAnimationGraph.cpp identifies both state
            // nodes and graph nodes.
            return static_cast<Object*>(obj)->Unk028_GetTI().typeName;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool TypeNameIs(const char* name, const char* expected)
    {
        return name && std::strcmp(name, expected) == 0;
    }

    void WalkStateNode(AtomStateNode* node, const std::function<void(AtomGraphStateNode*)>& cb,
        std::set<void*>& seen, int depth)
    {
        if (!node || depth > 24 || !seen.insert(node).second)
        {
            return;
        }
        const char* typeName = TypeNameOf(node);
        if (TypeNameIs(typeName, "AtomGraphStateNode"))
        {
            cb(static_cast<AtomGraphStateNode*>(node));
            return;
        }
        if (TypeNameIs(typeName, "AtomStateMachineNode"))
        {
            __try
            {
                auto* sm = static_cast<AtomStateMachineNode*>(node);
                for (uint16 i = 0; i < sm->States.size; ++i)
                {
                    WalkStateNode(sm->States.arr[i], cb, seen, depth + 1);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return;
        }
        if (TypeNameIs(typeName, "AtomLayeringStateNode"))
        {
            __try
            {
                auto* lay = static_cast<AtomLayeringStateNode*>(node);
                WalkStateNode(lay.BaseLayer.StateImplementation, cb, seen, depth + 1);
                for (uint16 i = 0; i < lay->BlendLayers.size; ++i)
                {
                    WalkStateNode(lay->BlendLayers.arr[i].StateImplementation, cb, seen, depth + 1);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        // AtomNullStateNode and other leaf state types have no children.
    }

    Animation* AnimOf(AtomGraphNode* node)
    {
        // AtomAnimationDataBaseNode::shared_Animation at +0x50 is a
        // SharedPtrNew<Animation>*; the managed object is behind the shared
        // block and carries Length at +0x48.
        uintptr_t sharedPtr = 0;
        if (!SafeRead<uintptr_t>(reinterpret_cast<uintptr_t>(node) + 0x50, sharedPtr) ||
            sharedPtr < 0x10000)
        {
            return nullptr;
        }
        __try
        {
            auto* block = reinterpret_cast<SharedPtrNew<Animation>*>(sharedPtr);
            Animation* anim = block->GetPtr();
            if (!anim)
            {
                return nullptr;
            }
            // Confirm we really are pointing at an Animation: its vtable is
            // Animation::GetVTBL() from ACU/Animation.h.
            uintptr_t vtbl = 0;
            if (!SafeRead<uintptr_t>(reinterpret_cast<uintptr_t>(anim), vtbl))
            {
                return nullptr;
            }
            return vtbl == Animation::GetVTBL() ? anim : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }
}

namespace AtomGraphQuery
{
    AtomGraph* GetPlayerAtomGraph()
    {
        // Same path the existing animation plugins use; every step is checked.
        __try
        {
            Entity* player = ACU::GetPlayer();
            if (!player)
            {
                return nullptr;
            }
            int idx = player->cpntIndices_157.atomAnimCpnt;
            AtomAnimComponent* ac = static_cast<AtomAnimComponent*>(player->cpnts_mb[idx]);
            if (!ac)
            {
                return nullptr;
            }
            return ac->shared_AtomGraph_NewDemo_DEV->GetPtr();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    AtomNodeKind ClassifyNode(AtomGraphNode* node)
    {
        const char* name = TypeNameOf(node);
        if (TypeNameIs(name, "AtomAnimationDataNode"))
        {
            return AtomNodeKind::Clip;
        }
        if (TypeNameIs(name, "AtomAnimationRootNode"))
        {
            return AtomNodeKind::Root;
        }
        if (TypeNameIs(name, "AtomTimeScaleNode"))
        {
            return AtomNodeKind::TimeScale;
        }
        return AtomNodeKind::Unknown;
    }

    void ForEachGraphState(AtomGraph* graph, const std::function<void(AtomGraphStateNode*)>& cb)
    {
        if (!graph || !graph->RootStateMachine)
        {
            return;
        }
        std::set<void*> seen;
        WalkStateNode(graph->RootStateMachine, cb, seen, 0);
    }

    void CollectClips(AtomGraph* graph, std::vector<ClipEntry>& out)
    {
        ForEachGraphState(graph, [&](AtomGraphStateNode* st)
        {
            __try
            {
                for (uint16 i = 0; i < st->Nodes.size; ++i)
                {
                    AtomGraphNode* node = st->Nodes.arr[i];
                    if (!node || ClassifyNode(node) != AtomNodeKind::Clip)
                    {
                        continue;
                    }
                    Animation* anim = AnimOf(node);
                    if (!anim)
                    {
                        continue;
                    }
                    ClipEntry entry;
                    entry.state = st;
                    entry.node = node;
                    entry.handle = static_cast<AtomAnimationDataNode*>(node)->shared_Animation->handle;
                    entry.length = anim->Length;
                    out.push_back(entry);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        });
    }

    namespace
    {
        // Follow one input port of type 9 (an output-port pointer) to the node
        // that produces this node's input. AtomInputPort::BindingType is at
        // +0x00 and the referenced port at +0x10; AtomOutputPort::graphNode is
        // at +0x00.
        AtomGraphNode* ProducerThrough(AtomGraphNode* node, int portIdx)
        {
            __try
            {
                if (node->InputPorts.size <= portIdx)
                {
                    return nullptr;
                }
                AtomInputPort& port = node->InputPorts.arr[portIdx];
                if (port.BindingType != 9)
                {
                    return nullptr;
                }
                AtomOutputPort* producerPort = port.outputPortIfTypeEq9_graphVarIdxIfEq0;
                if (!producerPort)
                {
                    return nullptr;
                }
                return producerPort->graphNode;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        // The animation root node binds its first port to whatever produces the
        // animation for this state: the clip itself, or a Node that wraps it
        // (AtomTimeScaleNode in this graph, per the runtime dump).
        AtomGraphNode* FindRootRecursive(AtomGraphNode* clip, AtomGraphNode* from, int depth)
        {
            if (!from || depth > 4)
            {
                return nullptr;
            }
            const char* name = TypeNameOf(from);
            if (TypeNameIs(name, "AtomAnimationRootNode"))
            {
                // Does this root play our clip, directly or through one wrapper?
                for (int hop = 0; hop < 3; ++hop)
                {
                    AtomGraphNode* producer = ProducerThrough(from, 0);
                    if (!producer)
                    {
                        return nullptr;
                    }
                    if (producer == clip)
                    {
                        return from;
                    }
                    from = producer;
                }
                return nullptr;
            }
            return nullptr;
        }
    }

    AtomGraphNode* FindRootForClip(AtomGraph* graph, AtomGraphNode* clipNode)
    {
        if (!graph || !clipNode)
        {
            return nullptr;
        }
        std::vector<AtomGraphNode*> roots;
        CollectRootNodes(graph, roots);
        return FindRootForClipIn(clipNode, roots);
    }

    AtomGraphNode* FindTimeScaleForClip(AtomGraph* graph, AtomGraphNode* clipNode)
    {
        if (!graph || !clipNode)
        {
            return nullptr;
        }
        std::vector<AtomGraphNode*> roots;
        CollectRootNodes(graph, roots);
        return FindTimeScaleForClipIn(clipNode, roots);
    }

    void CollectRootNodes(AtomGraph* graph, std::vector<AtomGraphNode*>& out)
    {
        ForEachGraphState(graph, [&](AtomGraphStateNode* st)
        {
            __try
            {
                for (uint16 i = 0; i < st->Nodes.size; ++i)
                {
                    AtomGraphNode* node = st->Nodes.arr[i];
                    if (node && ClassifyNode(node) == AtomNodeKind::Root)
                    {
                        out.push_back(node);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        });
    }

    AtomGraphNode* FindRootForClipIn(AtomGraphNode* clipNode, const std::vector<AtomGraphNode*>& roots)
    {
        if (!clipNode)
        {
            return nullptr;
        }
        for (AtomGraphNode* root : roots)
        {
            if (FindRootRecursive(clipNode, root, 0))
            {
                return root;
            }
        }
        return nullptr;
    }

    AtomGraphNode* FindTimeScaleForClipIn(AtomGraphNode* clipNode, const std::vector<AtomGraphNode*>& roots)
    {
        AtomGraphNode* root = FindRootForClipIn(clipNode, roots);
        if (!root)
        {
            return nullptr;
        }
        // Walk the producer chain from the root; a wrapper that is itself fed
        // by our clip is the timescale node.
        AtomGraphNode* current = root;
        for (int i = 0; i < 4; ++i)
        {
            AtomGraphNode* producer = ProducerThrough(current, 0);
            if (!producer || producer == clipNode)
            {
                return nullptr;
            }
            if (ClassifyNode(producer) == AtomNodeKind::TimeScale)
            {
                AtomGraphNode* itsProducer = ProducerThrough(producer, 0);
                if (itsProducer == clipNode)
                {
                    return producer;
                }
            }
            current = producer;
        }
        return nullptr;
    }
}
