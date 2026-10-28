# ACU Plugin Loader — ExamplePlugins Shared Progress

> **How to use this file**
> Both the design agent (Claude) and the coding agent read this at the start of every
> session to resume where we left off. After completing any task, update the relevant
> section before ending the session. Never delete history — append to it.

---

## Current Status: 🟢 3 plugins compiled, 3 working, 1 in discovery

| Plugin | Status |
|--------|--------|
| PreventAutoDrawPlugin | ✅ Working — prevents weapon unsheathe in combat (Steam), toggle F9 |
| ParryAssassinPlugin | ✅ Working — hooks assassination decision (0x140CD9BED/0x140CD9C95), 12-state combat check. Parry 2x → allows assassination for 4s |
| AnimationOverridePlugin | 🔶 Discovery only — hooks universal state dispatcher (0x1427555D2), counts assassination state entries for animation type mapping |
| ExactPositionTeleportPlugin | ✅ Working — A/B-VALIDATED STABLE (reset ON + breakfall-request OFF: 30+ min constant use, zero crashes). Breakfall-request layer = identified crash source. Force-freefall by same-position teleport + HSTE reset; Shift negated while mid-jump |

---

## Goal

A DLL plugin for ACU (Steam) that identifies which assassination animations play for each context (stealth, combat, air, ledge, etc.) and then allows forcing fast/short animations while suppressing slow/cinematic ones.

---

## Task Board

### ✅ Done
- [x] Stripped FreeJumpPlugin, ForceSheathePlugin, ParryAssassinPlugin from build
- [x] Created PreventAutoDrawPlugin — hooks close-range unsheathe decision at Steam addresses, toggle F9
- [x] Added DoForceSheathe() SEH-safe fallback for forced-equip scenarios
- [x] Created AllowSmokeAssassinatePlugin — hooks assassination disallow functions at 0x140CD9BED/0x140CD9C95
- [x] Fixed combat detection — 12 combat states now checked (was 3)
- [x] Wired all plugins into main.cpp and vcxproj

### 🔬 Research
- [x] Discovered universal state enter dispatcher at 0x1427555D2 (call site, safe to hook)
- [x] Mapped assassination state Enter addresses (PP=0x141A4A4B0, P=0x141A462F0, FirstHalf=0x141A42F60, SecondHalf=0x141A40BE0)
- [x] Confirmed Assassination_P hook at 0x141A462F0 works (same as ACUFixes)
- [ ] ~~GenericHooksInParkourFiltering~~ ABANDONED — not available standalone
- [ ] Map which animation type fires which state (Phase 1)
- [ ] Find safe call-site hooks for blocking states (Phase 2)

### 🏗️ Implementation
- [x] PreventAutoDrawPlugin — working
- [x] AllowSmokeAssassinatePlugin — working
- [x] AnimationOverridePlugin — partial (hook only, no override yet)
- [ ] Phase 1: Hook 0x1427555D2 to capture ALL assassination state transitions
- [ ] Phase 2: Add override logic (block slow states or modify AssassinAbilitySet flags)

### 🧪 Testing
- [x] PreventAutoDrawPlugin: auto-draw blocked, no crash on hit
- [x] AllowSmokeAssassinatePlugin: chase/search OK, combat blocked
- [ ] AnimationOverridePlugin: hook counts entries without crash
- [ ] Phase 1: run different assassination types, log which states fire

---

## Research Findings

### Approach: Universal State Enter Dispatcher (0x1427555D2)

Single call site where ALL human state Enter functions are dispatched. In callback:
- params->GetRAX() → Enter function being called
- params->rcx_ → FunctorBase* being entered

Compare RAX against known assassination Enter addresses to identify which animation type is about to play.

### State Addresses (Steam 1.5.x)

| Name | Enter Address | Role |
|------|--------------|------|
| Assassination_PP | 0x141A4A4B0 | Great-grandparent |
| Assassination_P | 0x141A462F0 | Parent state |
| Assassination_FirstHalf_mb | 0x141A42F60 | First animation half |
| Assassination_SecondHalf_mb | 0x141A40BE0 | Second animation half |

### Combat Detection (12-state check)

Checks primaryCallbackReceivers for any of 12 combat Enter addresses: Combat (0x1419BAA40), Combat_DoingNothing, Combat_BlockTooEarly, Combat_Parry_P, Combat_Parrying, Combat_Parrying_PunchEnemy, Combat_Stabbed_P, Combat_Stabbed, Combat_ChargedShove_Charging, Combat_ChargedShove_Shoving, Combat_Shoot_P, Combat_Shoot.

---

## Key Decisions

| Decision | Chosen approach | Reason |
|----------|----------------|--------|
| Steam vs Uplay | Steam addresses | User's game is Steam |
| Anim hook point | Universal dispatcher 0x1427555D2 | Function entries crash; this is a safe call site |
| Assassination_P hook | CCodeInTheMiddle, dontExecStolenBytes=true | ACUFixes-verified |
| Combat detection | 12-state vs 3-state | 3-state missed Combat_Shoot |
| Override approach | Phase 1 discovery, Phase 2 blocking | More reliable than Animation object swapping |

---

## Blockers

| Blocker | Status |
|---------|--------|
| FirstHalf/SecondHalf function entries crash | Workaround: use universal dispatcher instead |
| Need to map slow states first | Pending Phase 1 |

---

## Known Pitfalls

- Function entry hooks: CCodeInTheMiddle crashes unless dontExecStolenBytes=true; even then, some functions crash
- AnimationKey runtime modification: corrupts graph evaluation state
- MainConfig.cpp: global-constructor crash at DLL attach
- ImGui::SetCurrentContext: must be set in BOTH frame callbacks
- SharedPtrNew<T>: requires ->GetPtr(), not direct dereference
- ACU::GetPlayer(): safe, returns nullptr if unavailable

---

## Session Log

| Session | Agent | What was done |
|---------|-------|---------------|
| 1 | Coding | Created PreventAutoDrawPlugin, DoForceSheathe, AllowSmokeAssassinatePlugin (smoke-only) |
| 2 | Coding | Fixed AllowSmokeAssassinatePlugin signatures, changed to flat-out allow |
| 3 | Coding | Added 12-state combat check, created AnimationOverridePlugin stub |
| 4 | Coding | Stripped unsafe hooks, discovered universal dispatcher, updated this file |
| 5 | Coding | ExactPositionTeleport: toggleable HSTE reset (v0.3.2.0); A/B test — reset OFF = keypress no-op (reset is the mechanism); reset ON + breakfall OFF = VALIDATED STABLE 30+ min constant use, 0 crashes (v0.3.3.0: breakfall-request layer off by default, marked as crash source); added NegateShiftInSafeJump (clears game Shift/Sprint pressed-state while InSafeJumpOrLoweringToHang) to fix jitter/walking-in-air |

---

## Session 6 — ForcedSidehopPlugin wiring + crash root-cause (2026-10-28)

| Item | Detail |
|------|--------|
| Wired | ForcedSidehopPlugin is now the only gameplay plugin TU in ExamplePlugin (main.cpp + vcxproj). Dropped ExactPositionTeleport from the build (source kept). |
| First in-game test | Instant crash the moment any wall-climb parkour cycle ran with the plugin enabled (2/2 reproductions; plugin's own hotkey was NOT held either time — INI proves Hotkey=MouseX2). |
| Root cause | `src/GenericHooksInParkourFiltering.cpp` was the **ACUFixes-1.5.1** port, but the game is **1.5.0**. Its active patch was a 7-stolen-byte mid-function hook at 0x14013493E — a 1.5.1-derived address that lands on unrelated code in 1.5.0. Any parkour sort&select execution after the hook installed corrupted that code and crashed. `Enabled=1` persisted in the INI → hook active from startup. |
| Address evidence | Constant scan (8-byte LE) of the two reference DLLs that work on this install: ACUFixes.dll and ACU_ParkourPatches.dll both contain the 0.9.3 set — 0x140133B00 (sort&select start), 0x1401D1CF0/0x1401D1E50 (creation wrappers), 0x1401D4DE0, 0x1401D3000, 0x1401CC990, 0x14015A570, 0x142725F00 — and NONE of the 1.5.1 set (0x140134940, 0x14013493E, 0x1401D1260, 0x1401D13C0, 0x1401D4360, 0x1401D2580, 0x1401CBF00, 0x140159940, 0x142726000). FailedWallRunPlugin.cpp header independently dump-verifies 0x1401D1CF0/0x1401CC990. |
| Fix | 1) GPH sort&select is now the 0.9.3-validated **full function-start replacement** at 0x140133B00 (ReplaceFunctionAtItsStart, no stolen bytes), all DEFINE_GAME_FUNCTION addresses corrected to the 1.5.0 set, ParkourLog linked via `src/ParkourLogStubs.cpp` (pass-through stubs). GPH_Creation stays disabled. 2) ForcedSidehop: null-safe action-type read `GetTypeIntSafe()` (fancy idx 0x45 can be NULL — table 0x143A41930 — direct GetEnumParkourAction() calls crash; doc in FailedWallRunPlugin.cpp), RemoveGeneric address fixed to 0x142725F00. 3) Activation refuses (and says so in the UI) if 0x140133B00 is already patched by another plugin. Plugin version → 0.4.0.0. |
| Residual conflict | While the sidehop hook is installed it replaces ACU_ParkourPatches' own sort&select replacement → ParkourPatches helpers (dive/climbpass/toClimb/toFeet) pause; after disabling sidehop, ParkourPatches features may need a re-toggle to re-install their hook. Simplest test setup: remove/rename ACU_ParkourPatches.dll for sidehop test sessions. |
| Status | 🔬 Fix written, not yet compiled (GitHub Actions) and NOT runtime-verified |
| Retest plan | 1) With ACU_ParkourPatches.dll removed: enable sidehop, walk to wall, Shift+Space climb → must not crash. 2) On wall, hold Mouse X2 + Space → sideways hop; Mouse X2 + LCtrl → opposite hop. 3) Disable plugin → vanilla parkour unchanged. 4) With ACU_ParkourPatches.dll present: plugin UI must show the "site already patched" refusal instead of crashing. |

---

## Session 7 — Full FreeJump promoted to main plugin + INI persistence repair (2026-10-28)

| Item | Detail |
|------|--------|
| Copy selected | `ExamplePlugin/src/FreeJumpPlugin.cpp` is the only implementation that combines **Free Jump**, **Snap Direction**, and configurable per-key **Parkour Action Overrides**. `FreeJumpPlugin(2).cpp` is the older Free Jump + Center Screen variant; `DodgeAnywhere/.../FreeJumpPlugin.cpp` is camera-directed eject and has none of the requested override features. |
| Main wiring | Root `ExamplePlugin/src/main.cpp` and `ExamplePlugin/ExamplePlugin.vcxproj` compile and instantiate only `FreeJumpPlugin` as the gameplay plugin. The interposed `failedwallruntest1` commit (98ba273) had also added `FailedWallRunPlugin` to the same DLL; it is removed from the build again (source kept) because its three steal-sites (0x140135230/12B, 0x141A4D7CB/7B, 0x141A1BA0C/7B) collide byte-for-byte with ACUFixes-0.9.3 "Parkour helpers", are already hooked in the runtime dump, and relocated stolen `E9` bytes crash. `GenericHooksInParkourFiltering.cpp` and `ParkourLogStubs.cpp` remain required support units. Plugin version: 0.5.0.0. |
| INI root cause | The old loader read `FreeJumpKey=` from `substr(13)`, but the value begins at index 12; the first hex digit was discarded. More importantly, the root project did not compile or initialize this plugin, so its `LoadSettings()` never ran. Several override controls also failed to save, and the loader has no automatic deactivate callback. |
| Persistence fix | Prefix-based parsing (including the corrected `FreeJumpKey` offset), malformed-value tolerance (bad `NumOverrides` and bad `Override=` lines no longer discard already-read data; legacy files without `NumOverrides=` still load `Override=` lines; empty action-type field falls back to 50; count cap raised to 4096), hexadecimal Free Jump/Snap keys, decimal override keys for backward compatibility, persisted Snap Direction mode, eager saves for every persistent UI change, full-precision float output, and flush-before-replace writes (`.tmp` + `MoveFileExA`) to `Documents\Assassin's Creed Unity\FreeJumpPlugin.ini`. |
| Input fix | Replaced unreliable `GetAsyncKeyState` low-bit edge detection with per-frame high-bit rising-edge state. Only one rebind target can wait at a time; loader mouse clicks (VK 1–3) cannot be captured; the press that completes a rebind is latched (`m_JustCapturedKey`) so the new binding cannot trigger its own action on the capture frame. |
| 1.5.0 safety | Removed the action-pruning call to the ACUFixes-1.5.1 address `0x142726000` (dump shows it is the epilogue of the previous function); it now uses the ACUFixes-0.9.3 / ACU-1.5.0 address `0x142725F00`. Override action-type reads use the null-safe fancy-vtable check established by the ForcedSidehop crash investigation. New: activation refuses (and the UI reports) when the shared sort&select site 0x140133B00 is already patched by ACUFixes/ACU_ParkourPatches — dump shows `FF 25 …` installed there. |
| Static validation | `FreeJumpPlugin.cpp` and `main.cpp` pass Clang C++17 syntax checking with the project include paths; project XML parses; source list is FreeJump-only; `git diff --check` is clean. Isolated harnesses using the exact new load/save functions passed: (a) load → save → reload round-trip incl. `FreeJumpKey=1`, multi-type `102|65`, toggle state, fractional/full-precision fitness; (b) legacy compatibility (file without `NumOverrides=`, empty action type, malformed count preserving data). |
| Build/runtime status | Native MSVC v142 `Release|x64` build and in-game verification are still pending GitHub Actions and manual ACU testing. `GenericHooksInParkourFiltering.cpp` cannot be clang-validated (MSVC-only function→`void*` conversion at its `ReplaceFunctionAtItsStart` call, same pattern as ACUFixes-0.9.3). |
| Manual acceptance | 1) Launch ACU and confirm the UI shows the saved Free Jump, Snap Direction, and all override keys without reassigning them. 2) Restart ACU and confirm they remain. 3) Change every persistent control and restart again. 4) Exercise each override, including `Remove all other actions`, for crashes. 5) Test Free Jump and Snap Direction together with the loader menu closed and open. 6) With ACU_ParkourPatches.dll present, the UI must show the sort&select refusal text instead of silently overriding that plugin's hook. 7) Rebind Snap/Free Jump/override keys and confirm the action does not self-trigger from the rebind press. |

---

## Session 8 — ParryAssassin gold-highlight visual cue (2026-10-28)

| Item | Detail |
|------|--------|
| Feature | After 2 parries (ready window), the highlight glow around the enemy the player is currently attacking turns from white to **gold** — a visual queue alongside the existing ready-sound. |
| RE source | Offline analysis of the 1.5.0 runtime dump in `MODLOG/scripts/ACU_dumped_pe.exe` (Ghidra headless at `D:\ghidra`, JDK `D:\New folder\jbr`, scripts `DecompileRanges2.java`; outputs `highlight_system_decompile.txt`, `flag_writers_decompile.txt`, `bhv1430_check.txt`). Raw-dump RTTI/string/byte scans were done with throwaway Python one-liners (not saved). |
| Mechanism (verified in dump) | The enemy-highlight FX has named params `"critical color"` (Vector4, id cached at `0x145197D74`) and `"critical state"` (float, id at `0x145197D78`); ids resolved at startup by dynamic-initializer stubs `0x142D8E850`/`0x142D8E870` via `0x142759CF0`. `FUN_14087f850` (`0x14087F850..0x14087FA28`) re-spawns the highlight FX instance into the component's shared-ptr slot (R9) and applies params: color from its 6th arg via `mov r9,[rsp+0xB8]` → `call 0x141E30220` (SetParamVector) at `0x14087F9C9`, and the state float via `movss xmm3,[rsp+0xC0]` → `call 0x141E302D0` (SetParamFloat) at `0x14087F9F2`. Caller `FUN_14087fb32` (`0x14087FD04`) passes critical state `1.0f`/`0.0f` and picks the color from component bytes +0x515/+0x514 (statics `0x145197DA0` purple / `0x145197DB0` green) or from a per-target color object (+0x438 → +0x10). |
| Hook design | Two 8/9-byte mid-code hooks in `FUN_14087f850` at `0x14087F9B2` (steals `mov r9,[rsp+0xB8]`) and `0x14087F9E1` (steals `movss xmm3,[rsp+0xC0]`), `executeStolenBytes=false`, effects reproduced in the callbacks (`AllRegisters::GetRSP()` = hooked instruction's RSP, verified against the AutoAssemblerKinda trampoline: hook-entry `call` pushes one return address, saved-RAX pointer +0x18 compensates exactly). While `g_ParryAssaultEnabled` (the existing ready-window flag) is set, R9 is redirected to the configured gold RGBA and XMM3 forced to 1.0; otherwise the callbacks reproduce the stolen instructions verbatim (zero behavior change). |
| Target gating | Strict check (INI `StrictTargetCheck`, default on): the FX shared block being configured must be the player's `WhenHighlightedNPCChanges::highlightedNPCFxInstance` (`BhvAssassin+0x1430` → +0x28; raw SharedBlock* equality OR `SharedBlock::GetPtr()` equality). If the 0.9.3 offsets prove wrong at runtime, set `StrictTargetCheck=0` (then the tint applies to whatever highlight FX is being refreshed while ready). |
| Config | INI (`Documents\Assassin's Creed Unity\ParryAssassinPlugin.ini`): `HighlightCueEnabled` (default 1), `ForceCriticalState` (default 1), `StrictTargetCheck` (default 1), `GoldHighlightColor=1.000,0.840,0.000,0.000` (alpha 0 matches the vanilla special-color convention). ImGui: master checkbox + ColorEdit4 + the two toggles. |
| Wiring | `ParryAssassinPlugin.cpp/.h` re-added to `ExamplePlugin.vcxproj` and `main.cpp` (additive; FreeJump untouched), hooks installed in `InitStage_WhenCodePatchesAreSafeToApply` via `LoadSettings()`, gated per-callback. Plugin version → 0.6.0.0. |
| Static validation | Session-7 clang syntax-check method does NOT work on this machine (llvm-mingw headers `#error VARARGS not implemented`), and no MSVC/Windows SDK exists locally; correctness was established by re-reading the final sources against the verified dump disassembly. Hook steal-sites contain no `E9`/RIP-relative bytes (relocatable; FailedWallRun crash lesson respected). Sites are NOT hooked in the dump (original bytes present), so no collision with the user's currently-installed plugins. |
| Build/runtime status | ❌ NOT compiled, NOT runtime-verified. Build via the existing GitHub Actions workflow (push → `built-dlls` artifact). |
| Manual acceptance | 1) Enable Parry Assassination + cue; parry twice in combat → the attacked (highlighted) enemy's glow turns gold while the ready window is open and reverts afterwards. 2) Highlight a different enemy after 2 parries → gold follows the newly highlighted NPC. 3) Window expiry (8 s) or 2-parry timeout (4 s gap) → glow returns to white. 4) Toggle `HighlightCueEnabled` off → vanilla highlight only. 5) With `StrictTargetCheck=1`, Eagle Vision/other highlights must stay untinted. 6) No crash across highlight switching, entering/leaving combat, and desynchronization. If no gold appears at all: retest with `StrictTargetCheck=0`; if still none, the FX asset may ignore the color param — then try `ForceCriticalState=0`/=1 combinations and report back. |
| CI link fix | First Actions build failed: `LNK2001 unresolved external symbol "bool g_SmokeBombAssaultEnabled"` — ParryAssassinPlugin's assassination-decision hooks read that flag (allow-assassinate when ready **or** smoke-bomb dropped), and it is defined in `AllowSmokeAssassinatePlugin.cpp`, which Session 7 had stripped from the build. Re-added `AllowSmokeAssassinatePlugin.cpp/.h` to the vcxproj and wired it in main.cpp (OnUpdate/OnImGuiRender/LoadSettings). The plugin is checkbox-gated and defaults to OFF, so it sets the flag only for 7 s after a smoke-bomb drop when explicitly enabled; the assassination hooks already OR it with the parry-ready flag. |
