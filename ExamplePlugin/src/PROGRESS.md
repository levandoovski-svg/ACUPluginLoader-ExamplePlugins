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
