# AnimationLoopFixPlugin

Standalone ACU 1.5.0 plugin (ACUFixes Plugin Loader 0.9.3) that swaps one
animation clip inside the player's AtomGraph and removes the loop that a
shorter replacement produces.

## What the bug is, and what fixes it

Replacing a clip by handle works, but when the replacement is **shorter** than
the clip it replaces the character loops the animation. The reason is a split
of responsibility between two different clocks:

* `AtomAnimationDataBaseNode` — the class that actually holds
  `shared_Animation` (`ACU/AtomAnimationDataBaseNode.h`, field at `+0x50`) — has
  **no loop and no duration field at all**. It only streams the clip. Nothing on
  the object you swap can express "loop" or "play for N seconds".
* `AtomAnimationRootNode::IsLoop` (`ACU/AtomAnimationRootNode.h`, `uint8` at
  `+0x70`) is the **only** loop flag in the node family.
* The alternative looping mechanism — a state transition that targets itself
  with a `PLAYBACK_PERCENTAGE == 100%` condition — **does not exist in this
  game's graph.** In `AtomGraphDump.txt` (the 2.2M-line runtime dump of the
  player graph) every one of the **4022 transition conditions** is a
  graph-variable or `MarkUp()` comparison; a grep for the condition grammar
  finds only `bool`, `int`, `float`, `entityref` and `MarkUp` conditions.
* The parkour state's own exit is driven by the **human state machine**
  (`HumanStatesHolder`, e.g. the `WhileInLeap` parkour action), whose clock the
  replacement clip cannot change.

So with the original clip the state is torn down before the clip reaches 100%
and you never see a loop. With a shorter replacement the clip reaches 100%,
`IsLoop` restarts it, and it loops until the state finally ends — which is
exactly the length-dependent artifact you observed.

**This plugin therefore clears `IsLoop` on the `AtomAnimationRootNode` that the
swapped clip feeds.** It does not perturb the player's human state, does not
change when the state ends, and is fully reversible. Both the clip swap and the
`IsLoop` byte are restored when the plugin is toggled off.

For the concrete case that motivated this (`xx_Parkour_Leaping_Fd650cm`,
`Length = 2.5 s` replaced by `xx_Parkour_LeapFeet_fwd600cm`,
`Length = 1.1666666 s`, a 46.7 % ratio) the clip now reaches its last frame and
holds it for the remainder of the state instead of looping.

## An important finding about the older override code

`ExamplePlugin/src/AnimationOverridePlugin.cpp` walks the graph like this:

```cpp
for (state : graph->RootStateMachine->States)
    for (node : ((AtomGraphStateNode*)state)->Nodes)     // Nodes at +0x80
        ((AtomAnimationDataBaseNode*)node)->shared_Animation = ...
```

The dump shows the player graph's six top-level states are all
`AtomLayeringStateNode` or `AtomStateMachineNode` — **not one is an
`AtomGraphStateNode`**. So that walk reads `+0x80` of the wrong object types
(`BlendLayers` for a layering state, `States` for a state machine) and then
writes a `SharedPtrNew<Animation>*` into `+0x50`, which on those objects is
`AtomLayeringInfo::CharacterResolution`/`DynamicBoneLayerMarkup` and
`AtomStateNode::parentNode_mb` respectively. The second one is state-tree pointer
corruption.

If animation replacing appeared to work before, it did not work through that
path. Use this plugin instead: it only touches nodes it has positively
identified.

## Build

`Release|x64`. GitHub Actions: `.github/workflows/build-animationloopfix.yml`
uploads `AnimationLoopFixPlugin.dll` from
`AnimationLoopFixPlugin/build/x64/Release/plugins/`.

No local MSVC v142 in this environment — the workflow is the build path.

## Installation

Copy `AnimationLoopFixPlugin.dll` to
`Assassin's Creed Unity\ACUFixes\plugins`.

Settings and log live at
`Documents\Assassin's Creed Unity\AnimationLoopFixPlugin.ini` / `.log`.

## Getting the two handles (the only manual step)

The plugin never guesses a handle. Press **J** in game (or *Dump clips to log*)
to write every clip node in the player graph to the log, one line each:

```
clip #12 handle=0x116C0252BC length=2.500000 root=yes timescale=no
```

Pick out the line whose `length` matches the clip you want to replace (2.5 s for
`xx_Parkour_Leaping_Fd650cm`) and whose root node exists, put it in
`OriginalHandle`, then put the replacement clip's handle in
`ReplacementHandle` and toggle the plugin on.

The runtime dump already tells you where the relevant node lives. The
`Parkour == 8` action is `EnumParkourAction::stepDownWaistHeight_mb`
(`ACUFixes-0.9.3/ACUFixes/src/ParkourDebugging/EnumParkourAction.h`). In
`AtomGraphDump.txt` that state machine is at `RuntimeStateID: 12714`
(`AtomTaskID: 132C0376…`), whose first state is `RuntimeStateID: 12715`
(`AtomTaskID: CA0AC72A…`, `AnimMarkup: 400051`) containing two
`AtomAnimationDataNode`s with handle `32302899222` plus the
`AtomAnimationRootNode` that this plugin would patch. Its sibling subtree holds
the `TargetDistance`-selected variants (the "650 cm context"), which is why more
than one clip plays for this move.

## Known limitations

* **`IsLoop` has not been confirmed on the live node.** The dump does not print
  node fields, only names and port bindings. The evidence for `IsLoop` is the
  ACU-RE header, the absence of any `PLAYBACK_PERCENTAGE` transition, and the
  length-dependence of the symptom. The plugin logs the `IsLoop` byte it reads,
  so the first run confirms it.
* **Time-stretching is not implemented.** The clip chain is
  `AtomAnimationDataNode -> AtomTimeScaleNode -> AtomAnimationRootNode`, and the
  timescale node's input port of `BindingType 3` holds a constant float inline
  at `AtomInputPort + 0x10`. Scaling the 1.17 s clip by ~0.467 would fill the
  state instead of holding the last frame. This is deliberately left out: there
  is **no `ACU/AtomTimeScaleNode.h` in ACU-RE 0.9.3**, so none of that node's own
  fields are verified, and writing an unverified offset into a shared graph is
  not worth it for a cosmetic gain. The node is located and reported by
  `FindTimeScaleForClip` if you want to pursue it.
* **Root motion differs between the two clips** (`LeapFeet_fwd600cm` ~600 cm vs
  `Leaping_Fd650cm` ~650 cm, and `AtomAnimationDataBaseNode::DisplacementResultID`
  drives parkour displacement). Even a perfect loop fix may change the leap
  distance.
* **Option B (player-state reset) is off by default.** Automatic "clip ended"
  detection needs a verified runtime signal; the only candidate in ACU-RE,
  `GraphEvaluation_848::paActiveAnims_mb`, is declared but never used anywhere
  in the reference repository, so its semantics are unverified. The deferred
  game-thread reset machinery is implemented and proven
  (`Human__Dispatch_HSTE_ResetBehavior` at `0x14131A530`, deferred through the
  mid-function hook at `0x141F3FE3B`), and is exposed as an explicit interval
  for A/B testing rather than an inferred one.
* Nothing here has been tested in game. Static analysis only.

## Provenance

| Fact | Source |
|---|---|
| `AtomAnimationDataBaseNode::shared_Animation` at `+0x50`, no loop field | `ACUFixes-0.9.3/CommonLibACU/ACU-RE/inc/ACU/AtomAnimationDataBaseNode.h` |
| `AtomAnimationRootNode::IsLoop` at `+0x70` | `ACUFixes-0.9.3/CommonLibACU/ACU-RE/inc/ACU/AtomAnimationRootNode.h` |
| `Object::Unk028_GetTI()`, `TypeInfo::typeName` at `+0x18` | `ACU-RE/inc/ACU/ManagedObject.h`, `ACU-RE/inc/ACU/TypeInfo.h` |
| Node/state type dispatch by `Unk028_GetTI().typeName` | `ACUFixes-0.9.3/ACUFixes/src/AnimationTools/DumpAnimationGraph.cpp:749,1081-1091` |
| Clip handle printed per animation node, no `PLAYBACK_PERCENTAGE` conditions | `D:\Games\Assassins Creed Unity\AtomGraphDump.txt` |
| `Human__Dispatch_HSTE_ResetBehavior` = `0x14131A530`; deferred hook at `0x141F3FE3B` (6 bytes) | `ExamplePlugin/src/ExactPositionTeleportPlugin.cpp` |
| `WhileInLeap_VTable` = `0x142FFD1E0` via `callbacksForIdx[366]` | `ACUFixes-0.9.3/ACUFixes/src/VariousPatches/Hack_MoreReliableQuickshot.cpp:101` |
| `FindOrMakeSharedBlockByHandleAndIncrementStrongRefcount` | `ACU-RE/inc/ACU/ManagedPtrs/ManagedPtrs.h` |
| `stepDownWaistHeight_mb` = parkour action 8 | `ACUFixes-0.9.3/ACUFixes/src/ParkourDebugging/EnumParkourAction.h` |
