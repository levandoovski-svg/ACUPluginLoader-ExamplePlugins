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
