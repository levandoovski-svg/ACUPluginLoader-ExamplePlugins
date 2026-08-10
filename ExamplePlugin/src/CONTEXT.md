# Breakfall Plugin — Project Context

## Goal
A plugin for Assassin's Creed Unity (v1.5.0) that forces `UsingLift_Falling` state on keypress to enable breakfall anytime, bypassing animation/input locks that normally prevent the player from breaking falls.

---

## Constraints & Preferences

- Must override regardless of animation playing or input being blocked ("force logical states")
- No CE interaction from user — CE MCP handles memory work, user handles in-game triggers only
- Plugin built on **ACUFixes / ExamplePlugin** infrastructure (ACUPluginLoader)
- Game version: **ACU 1.5.0** (Steam, final patch)

---

## Architecture Overview

### Plugin Infrastructure

The plugin system uses **ACUPluginLoader** which loads DLL plugins from `plugins/` directory. Each plugin exports `ACUPluginStart()` which registers callbacks:

```
main.cpp
  └─ ACUPluginStart()
       ├─ InitStage_WhenSafe    → g_Handgrenade.LoadSettings()
       ├─ EveryFrameEvenWhenMenuIsClosed → g_Handgrenade.OnUpdate()
       └─ EveryFrameWhenMenuIsOpen → g_Handgrenade.OnImGuiRender()
```

### Current Files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Plugin entry point, instantiates `HandgrenadePlugin`, wires callbacks |
| `src/FreeJumpPlugin.cpp` | Core plugin: grenade swap, state scanner, force-state logic, ImGui UI |
| `src/FreeJumpPlugin.h` | Header: `HandgrenadePlugin` class + `StateCtorInfo` struct |
| `src/SharedStateEnterDispatcher.cpp` | Hooks 0x1427555D2, dispatches Enter callbacks |
| `src/SharedStateEnterDispatcher.h` | Header: static subscriber pattern for Enter hook |
| `src/pch.h` | Precompiled header: Windows, ImGui, ACU types |
| `src/exports.def` | DLL exports definition |
| `ExamplePlugin.vcxproj` | Project file (MSBuild) |

### How the Hook Works

The single call site at `0x1427555D2` dispatches ALL human state Enter functions:

```asm
call rax   ; rax = Enter function pointer loaded from FunctorBase+0xE0
```

`SharedStateEnterDispatcher` hooks this with `AutoAssemblerKinda` (7-byte hook, `CCodeInTheMiddle` with `RETURN_TO_RIGHT_AFTER_STOLEN_BYTES`). In the callback:
- `params->rax_` → the Enter function about to be called (can be rewritten)
- `params->rcx_` → the `FunctorBase*` node being entered

### FunctorBase Structure (from HumanStatesHolder.h)

```
Offset  Field
0x0008  directChild_mb
0x0010  pendingDirectChild
0x0018  fancyVTable (FancyVFunction*)
0x0028  parentStack (SmallArraySemistatic)
0x00B8  someNodes_B8
0x00C4  nonoverridingChildren
0x00E0  Enter (function pointer)
0x00E8  Exit (function pointer)
0x0100  Total size
```

### State Creator/Node Pattern

States are constructed via a factory chain:
1. Constructor allocates node (HeapAlloc, size varies per state)
2. Fills `Enter` (+0xE0) and `Exit` (+0xE8) with function pointers
3. Sets `fancyVTable` (+0x18) — used for state-specific dispatch
4. The generic Enter callsite at 0x1427555D2 calls `node->Enter(node)` via `call rax`

---

## CE-Based Reverse Engineering — Phase 1 Status

### Current Blockers for Finding player_base (1.5.0)

The `ACU-RE` library contains **24 hardcoded singleton pointer addresses** (e.g., `CameraManager::GetSingleton()` reads `*(CameraManager**)0x14521AAD0`). **Every single one is stale for ACU 1.5.0**. The values at these addresses do not contain valid pointers to the expected objects.

The pointer chain to `HumanStatesHolder`:
```
CameraManager::GetSingleton()  → *(CameraManager**)0x14521AAD0 [STALE]
  └─ camMgr->arr_to_ACUPlayerCameraComponent[0]
       └─ camCpnt->entity  → Entity* (player)
            └─ cpnts_mb[atomAnimCpnt] → AtomAnimComponent
                 └─ human_c58 → Human*
                      └─ humanStates → HumanStatesHolder*
```

Since `CameraManager` address is stale, `HumanStatesHolder::GetForPlayer()` returns `nullptr`.
The plugin handles this gracefully (returns early in `OnUpdate()` if null).

### CE MCP Tools — Verified Working

| Tool | Status | Notes |
|------|--------|-------|
| `launch_and_attach` | ✅ | Works for attaching to ACU.exe |
| `get_status` | ✅ | Returns CE status |
| `get_module_list` | ✅ | Returns loaded modules |
| `read` / `read_bytes` | ✅ | Memory reading, used to verify addresses |
| `write` | ✅ | Memory writing |
| `scan` / `refine_scan` | ✅ | Exact-value float/int scans |
| `freeze` | ✅ | Lock memory values |
| `resolve_pointer_chain` | ✅ | Multi-level chain resolution |
| `find_what_writes` / `find_what_accesses` | ✅ | Hardware breakpoint tracing |

### CE MCP Tools — Verified Not Working

| Tool | Issue | Root Cause |
|------|-------|------------|
| `find_player_base` | Fails | Uses stale pointer chain for pre-1.5.0 |
| `snapshot_diff` | Fails | Depend on player_base |
| `watch_parkour_states` | Fails | Depends on player_base |
| `map_player_struct` | Fails | Depends on player_base |
| `aob_scan` | Times out (15s) | Lua execution timeout, even with `module` filter |
| `save_param_offset` | N/A | Depends on player_base |

### CE MCP — Unused Tools Worth Trying
- `disassemble` — disassembly from an address
- `watch` — poll an address for changes over time

### Next Steps for CE Phase 1
1. Get a **known player value** to scan for (health, coordinate, etc.)
2. Use `scan` + `refine_scan` to isolate the address
3. Use `find_what_writes` with hardware breakpoint to trace back
4. Identify the correct static pointer address for 1.5.0
5. Resolve the full pointer chain
6. Verify by reading live-changing values through the chain

---

## Force State Approaches — History

### Approach 0: Original Plugin (pre-context)
The plugin started as "HandgrenadePlugin" — swaps poison bomb projectile for mortar bomb projectile on keypress. The force-state feature was added to this same plugin.

### Approach 1: Construct + Set pendingDirectChild (FAILED)
**Idea**: Allocate a new node via `HeapAlloc`, find the constructor via LEA+mov pattern in `.text`, call constructor, then set `pendingDirectChild` on the state machine parent.

**Result**: Crash at `Enter+0x21` — the node was constructed on raw `HeapAlloc` memory missing the full factory chain. The constructor only does partial init; additional fields are filled by the caller after constructor returns. Incomplete node = crash.

### Approach 2: Repurpose Leaf In-Place (FAILED)
**Idea**: Find an existing leaf node in the state tree, rewrite its fields (Enter, Exit, fancyVTable) to match the target state (UsingLift_Falling). No alloc needed.

**Result**: Crash at offset `0x275CBEC` — subclass-specific fields beyond the base `FunctorBase` (0x100 bytes) don't match the layout expected by UsingLift_Falling's Enter function. The node has stale data from its original class.

### Approach 3: Hook Enter Callsite, Redirect RAX (CURRENT)
**Idea**: At the generic dispatch point (0x1427555D2), when the key is pressed, rewrite `params->rax_` to point to the target state's Enter function. The node itself is valid — we just change which Enter runs on it.

**Mechanism**:
```
g_WantRedirect = true;
g_RedirectEnter = m_TargetEnter;  // 0x141A3D2D0 (UsingLift_Falling Enter)
```

In `BreakfallEnterHook()`:
1. Check if `g_WantRedirect` is set
2. Verify the node is a top-level state (parentStack[topmost] == HumanStatesHolder root)
3. Overwrite `*params->rax_` with `g_RedirectEnter`
4. Clear `g_WantRedirect`

**Status**: Active development. Two iterations:
1. **Initial**: Redirected on ANY Enter call — crashed immediately with AV at `Enter+0x4B` (141A3D31B) because the hook caught a non-player state transition (code path 142102282 → 14274F6AD), and UsingLift_Falling's Enter ran on an NPC/node with incompatible data layout.
2. **Current (fixed)**: Added player-origin check — only redirect when node's root parent (`parentStack[topmost]`) matches `HumanStatesHolder::GetForPlayer()`. This rejects NPC/animals/other entity transitions.

**Build issue**: `SharedStateEnterDispatcher.cpp` was missing from `ExamplePlugin.vcxproj` `<ClCompile>` items, causing LNK2001 unresolved external for `Initialize` and `Subscribe`. Fixed by adding the file to the project.

**Known Concern**: This approach only fires during **natural state transitions**. The hook callback only triggers when the game dispatches an Enter call. If the player is in a locked animation (no transition happening), pressing the key sets the flag but no redirect occurs until the next natural transition. This may need a "force state update" mechanism.

---

## Key Decisions

| Decision | Choice | Reason |
|----------|--------|--------|
| **Game version target** | ACU 1.5.0 Steam | User's game |
| **Plugin framework** | ACUPluginLoader / ACUFixes | Existing infrastructure, proven hooks |
| **Hook approach for redirect** | `CCodeInTheMiddle` at 0x1427555D2 | Safe call site (not function entry), ACUFixes-verified |
| **RAX redirect** vs constructor/repurpose | RAX redirect | Both constructor and repurpose crashed — node data incomplete |
| **Thread safety** | Static `g_WantRedirect` + `g_RedirectEnter` flags | Avoids thread-safety and reentrancy issues |
| **Node validation** | Check `parentStack` topmost == HumanStatesHolder | Prevents redirecting on non-player states |
| **Scanner** | Runtime LEA+mov pattern scan in `.text` | Finds all state constructors without hardcoded addresses |
| **Settings storage** | `Handgrenade.ini` in Documents | Standard ACU mod pattern |
| **ImGui integration** | Separate frame callbacks for menu open/closed | ACUPluginLoader convention |
| **Weapon swap** | Exploit poison bomb projectile pointer | Existing feature, useful testbed for pointer manipulation |

---

## Known Issues

1. **Redirect timing** — RAX redirect only fires during natural transitions. If no transition occurs, the keypress has no visible effect. A mechanism to force a state update may be needed.

2. **ACU-RE addresses are ALL stale** for 1.5.0 — every hardcoded singleton address in the library is wrong. `HumanStatesHolder::GetForPlayer()` returns nullptr.

3. **Build environment** — No MSBuild installed locally. Builds must be done via GitHub Actions. This means test iterations are slow.

4. **AOB scanning in CE MCP times out** — cannot scan for LEA patterns to verify 1.5.0 addresses.

5. **`find_player_base` CE tool** — uses hardcoded patterns for an older ACU version, fails on 1.5.0.

6. **Crash at Enter+0x21** — constructor-based approach failed because HeapAlloc only provides raw memory; the game's factory chain does additional setup beyond the constructor call.

7. **Crash at 275CBEC** — repurposing an existing leaf node failed because subclass-specific fields contain stale data incompatible with the target state.

8. **Crash at Enter+0x4B** — redirect approach without player-origin check crashes because the dispatch callsite at 1427555D2 is shared by ALL entity state machines (NPCs, animals, etc.). UsingLift_Falling's Enter function ran on a non-player node with incompatible data layout.

9. **`SharedStateEnterDispatcher.cpp` not in vcxproj** — caused LNK2001 unresolved externals for `Initialize` and `Subscribe`. Fixed by adding `<ClCompile Include="src\SharedStateEnterDispatcher.cpp" />`.

10. **Project fragmentation** — The plugin (HandgrenadePlugin/FreeJumpPlugin) contains both the handgrenade weapon swap feature AND the force-state breakfall feature. These are unrelated and could conflict.

---

## Addresses (ACU 1.5.0, Verified at Runtime)

### State Machine
| Address | Description |
|---------|-------------|
| `0x14274F4D0` | `TransitionChild` function |
| `0x1427555D2` | Generic Enter dispatch callsite (`call rax`) — hooked by SharedStateEnterDispatcher |
| `0x141A3D2D0` | `UsingLift_Falling::Enter` — the target state we want to force |

### State Constructors (LEA+mov pattern)
Found via runtime scanner in FreeJumpPlugin (exact addresses depend on 1.5.0 binary layout):
```
Enter=0x141A3D2D0  Exit=0x141A3D560  Ctor=0x...  Size=...
```
(Full list populated at runtime by `RunScannerOnce()`)

### FunctorBase Layout
```
Enter    → +0xE0 (function pointer)
Exit     → +0xE8 (function pointer)
fancyVTable → +0x18 (FancyVFunction*)
directChild → +0x08
parentStack → +0x28
```

---

## CE MCP — Capability References

### Required for Future RE (not currently available in MCP)
- AOB scanning in `.text` (RX only) — needed to find constructor patterns
- Call stack capture — needed to understand crash context
- Register readout on breakpoint hit — needed for `find_what_writes` context
- Bulk memory read for struct mapping — partial via `read_bytes`
- Hardware breakpoints — ✅ available via `find_what_writes` / `find_what_accesses`
- Module enumeration — ✅ available via `get_module_list`
- Disassembly — ✅ available via `disassemble`

---

## Project Dependencies

### External Libraries (in CommonLibACU/)
| Library | Purpose |
|---------|---------|
| `ACU-RE` | ACU struct definitions, singleton accessors (stale for 1.5.0) |
| `AutoAssemblerKinda` | Code hooking framework (CCodeInTheMiddle, AutoAssembleWrapper) |
| `Common_PluginSide` | Plugin API types, ACUPlugin interface |
| `DearImGui` | In-game UI rendering |
| `Serialization` | Settings serialization |

### NuGet Dependencies (from ACUFixes)
- DirectXTK
- Detours
- ImGui

---

## File Inventory

```
ExamplePlugin/
├── ExamplePlugin.vcxproj          # Project file
├── AtomGraphDump.txt              # Debug dump
├── .gitignore
└── src/
    ├── main.cpp                   # Entry point, callback registration
    ├── FreeJumpPlugin.cpp         # Core plugin logic (492 lines)
    ├── FreeJumpPlugin.h           # HandgrenadePlugin class + StateCtorInfo
    ├── SharedStateEnterDispatcher.cpp  # Hook at 0x1427555D2 (45 lines)
    ├── SharedStateEnterDispatcher.h    # Subscriber pattern header
    ├── pch.cpp                    # Precompiled header stub
    ├── pch.h                      # PCH: Windows, ImGui, ACU types
    ├── exports.def                # DLL exports
    ├── PROGRESS.md                # Session log (original, now stale)
    └── CONTEXT.md                 # This file
```

---

## Session Log

| Date | Agent | What was done |
|------|-------|---------------|
| Session 1 | Coding | Created PreventAutoDrawPlugin, DoForceSheathe, AllowSmokeAssassinatePlugin |
| Session 2 | Coding | Fixed AllowSmokeAssassinatePlugin, changed to flat-out allow |
| Session 3 | Coding | Added 12-state combat check, created AnimationOverridePlugin stub |
| Session 4 | Coding | Discovered universal dispatcher at 0x1427555D2 |
| Session 5 | Design | Mapped state constructor chain, identified FunctorBase layout |
| Session 6 | Design | Failed: constructor approach (crash at Enter+0x21) |
| Session 7 | Design | Failed: repurpose leaf approach (crash at 275CBEC) |
| Session 8 | Design | Created redirect approach with g_WantRedirect / g_RedirectEnter |
| Session 9 | Design | CE RE session — confirmed all ACU-RE addresses stale for 1.5.0 |
| Session 10 | Coding | Fixed LNK2001 (added SharedStateEnterDispatcher.cpp to vcxproj); redirect crashed at Enter+0x4B (non-player transition); added player-origin check via parentStack root comparison; updated CONTEXT.md |
