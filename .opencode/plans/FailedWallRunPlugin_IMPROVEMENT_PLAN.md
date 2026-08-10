# FailedWallRunPlugin — Revised Strategy: Direct RTCP Override + GlobalTransitions

> Communication document between opencode and antigravity agents.

---

## Goal

Make ALL parkour actions cancelable mid-animation with Ezio-level responsiveness. When the player provides new input, instantly transition to the appropriate new action.

---

## Critical Bug Fix (Phase 0 — Prerequisite)

**RTCP index is wrong.** The graph dump (`AtomGraphDump.txt`) proves `int Parkour` is RTCP index **372**, not **403** (which is `scalar PusherToVictimAngle`).

- `FindParkourSM` searches nodes for RTCP 403 — will never find the parkour SM
- The entire animation graph interrupt feature is dead code without this fix

**Fix:** Change `NodeUsesRTCP(node, 403)` to `NodeUsesRTCP(node, 372)`.

---

## Approach: Two-Pronged Strategy

### Prong A: Direct RTCP Override (Ezio-style)
When new parkour input arrives mid-animation, directly write the desired `Parkour` value to RTCP index 372. The parkour dispatch SM (`B4CFDA20`, RuntimeStateID: 10414) already uses this variable to route to the correct animation — it's the game's built-in mechanism for selecting parkour actions. By writing to it directly, we bypass the waiting-for-animation-markers behavior.

### Prong B: GlobalTransition Safety Net
The Main Layer SM (`B8D76890`) has 30 GlobalTransitions, but none handle "interrupt from any parkour substate". Add one GlobalTransition that fires when our custom RTCP variable is set, targeting the parkour dispatch state.

---

## Key Graph Dump Findings

### State Machine Hierarchy

```
RootStateMachine (B9438AE0, line 2)
└── State #0: AtomLayeringStateNode
    └── Main Layer SM (B8D76890, line 103)     ← INJECT GLOBALTRANSITION HERE
        ├── 30 GlobalTransitions
        ├── Parkour transitions target State #4
        └── State #4: Parkour Dispatch SM (B4CFDA20, line 190963)
            ├── 5 GlobalTransitions (all reference Parkour RTCP 372)
            ├── 6 states for specific parkour actions
            └── Routes based solely on `int Parkour` value
```

### Target State

The Main Layer SM at `B8D76890` has 30 GlobalTransitions. ALL parkour-related transitions — for values 0 (idle), 17, 88, 98, 124, etc. — target **State #4**. This is the parkour dispatch/evaluation state.

**`TargetStateIndex = 4`** is the correct target for our cancel GlobalTransition.

### RTCP Variable Mapping

| Index | Name | Type |
|-------|------|------|
| 372 | Parkour | int |
| 403 | PusherToVictimAngle | scalar (NOT Parkour) |
| 375 | ParkourMode | int |
| 525 | WallEject | bool |
| 440 | SlopeScramble | bool |
| 441 | SlopeSlide | bool |

### Parkour Value Map

| Value | Likely Meaning |
|-------|----------------|
| 0 | No parkour / idle on wall |
| 1 | Wallrun |
| 6, 87 | Side eject |
| 17, 112 | Vault forward |
| 31 | Wall eject |
| 55, 2, 8, 10, 12, 14, 18, 20 | Various parkour substates |
| 57, 58 | Slope scramble/slide |
| 88, 98 | Ledge grab / vault |
| 124 | Cancel / drop |

---

## Implementation Plan

### Phase 1: Fix FindParkourSM and Target State

**In `FailedWallRunPlugin.cpp`:**
- `FindParkourSM`: Change `NodeUsesRTCP(node, 403)` → `NodeUsesRTCP(node, 372)`
- When injecting GlobalTransition: Change `tr->TargetStateIndex = 0` → `tr->TargetStateIndex = 4`

### Phase 2: Add Direct RTCP Override (SetParkourRTCP)

Add a function to write directly to the Parkour RTCP variable:

```cpp
void SetParkourRTCP(AtomGraph& graph, int32 value)
{
    if (!graph.rtcp) return;
    uint32 offset = graph.rtcp->graphVarsOffsets[372];
    *(int32*)(graph.rtcp->graphVarsBuffer.arr + offset) = value;
}
```

When new parkour input is detected mid-animation (`GeneralState == 5`):
1. Set `ForceCancelAnim` RTCP to trigger GlobalTransition
2. Set `Parkour` RTCP (372) to 0 to force re-evaluation of dispatch SM
3. Next frame: graph evaluates dispatch SM initial states, routes based on Parkour==0

### Phase 3: Two-Frame Clear Pattern

Replace current same-frame set/clear with deferred clear:

```cpp
// Frame 1: Set both
SetGraphVariable(*graph, m_ForceCancelRTCPIdx, true);
SetParkourRTCP(*graph, 0);
m_NeedsClearRTCP = true;

// Frame 2: Clear
if (m_NeedsClearRTCP)
{
    SetGraphVariable(*graph, m_ForceCancelRTCPIdx, false);
    m_NeedsClearRTCP = false;
}
```

### Phase 4: Gate on Parkour State

Only trigger the interrupt when the player is actually in a parkour state (`GeneralState == 5`). Check this via `HumanStatesHolder` or by reading `GeneralState` RTCP variable (index 237).

### Phase 5: Cleanup on Deactivate

Remove injected GlobalTransitions when the feature is toggled off or the plugin unloads. Track the injected condition expression pointer to identify and remove the correct transition.

---

## Open Questions

1. **Graph sharing** — AtomGraph may be shared via SharedPtr. If NPCs use the same graph, our GlobalTransition injection affects them too. If issues arise, clone the graph.
2. **Blend quality** — `0.15f` TransitionTime may cause visual popping; make configurable.
3. **GeneralState check** — Need to reliably read `GeneralState` RTCP (index 237) to gate the interrupt. The current code doesn't check if the player is actually in parkour before triggering.

---

## Next Steps

1. Fix RTCP index from 403 → 372
2. Fix TargetStateIndex from 0 → 4
3. Add SetParkourRTCP direct override
4. Fix two-frame clear pattern
5. Gate on GeneralState == 5
6. Add cleanup hooks
7. Build via GitHub Actions, test in-game
