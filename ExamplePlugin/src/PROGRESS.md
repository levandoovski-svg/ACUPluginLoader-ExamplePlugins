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

---

## Session 9 — Parkour Mode + Gamepad (XInput) support + MMB binding (2026-10-29)

| Item | Detail |
|------|--------|
| Feature 1: Parkour Mode | Bindable toggle key (default F7). While ON, the selected `ActionKeyCode` entries are forced unpressed every frame, freeing their physical keys for override bindings. Replaces the user's earlier idea of editing `AssassinAbilitySet` ability bytes: the per-action "keymap" pipeline is verified end-to-end from the 1.5.0 runtime dump, needs no state save/restore, and never modifies the player's real bindings. |
| Input pipeline RE (1.5.0 dump, verified) | Frame input update fn = `0x14273BAC0` (vfunc slot 0x0C; vtable slot at `0x14363D660`, class owns the keymap = its own `rdx` arg). Order inside it: poll raw input (`0x142728C40` scancodes/`0x142728F20` mouse) → prevFrame=thisFrame, zero `InputContainerBig::keyStates_thisFrame` (`+0x30`, 32 bytes = `ActionKeyCode::ACTION_SET_SIZE`) → **keymap fill `0x14273B2B0`** (rcx=InputContainerBig*, rdx=MouseState copy, r8=isPressed_byScancode[256], r9=keymap; loop `r11d=0x20`, entry stride `0x214`, type 1 = keyboard scancode with special left/right-ctrl(0x1D/0x9D)/shift(0x2A/0x36)/alt(0x38/0xB8) handling, type 2 = `mouseState+0xC` button index) → mouse-vector fn `0x14273C190` (reads keymap axis entries at +0x4280/+0x4494/+0x46a8/+0x48BC/+0x4EF8/+0x510C/+0x4AD0/+0x4CE4) → bool→float conversion at keymap fn tail (`isPressed_byAction[1]→fltIsPressed_LMB@0x274`, `[11]→RMB@0x29C`, `[14]→Reload@0x2A8`) — all matches `ACU-RE`'s `InputContainer` layout on 1.5.0. |
| Parkour Mode hook | 6-byte `CCodeInTheMiddle` at `0x14273BC7B` (vanilla bytes `48 8B D6 48 8B CB` = `mov rdx,rsi; mov rcx,rbx`, read+asserted before install), i.e. between the keymap fill and the mouse-vector update. Callback reads `params->rbx_` (the `InputContainerBig*`, same register the ACUFixes-0.9.3 mousewheel-cycle hook at `0x14273BC75` uses for the same struct) and zeroes `keyStates_thisFrame[i]` for each disabled action. Chains cleanly *behind* the installed ACUFixes hook: their trampoline returns to `0x14273BC7B`, ours returns to `0x14273BC81`. Lazily activated/deactivated with the toggle (bytes restored on deactivate), so no footprint when the mode is unused. |
| Disabled actions UI | All 24 named `ActionKeyCode` entries as checkboxes (defaults: LMB attack, RMB aim, Crouch), plus "Restore defaults". Persisted as `ParkourDisabledActions=1\|11\|10` (decimal, pipe-separated indices). |
| Feature 2: Gamepad | XInput via dynamic `LoadLibrary` chain (xinput1_4 → 1_3 → 9_1_0 → 1_2 → 1_1, named export + ordinal-100 fallback; the dump shows ACU itself imports `xinput1_3.dll`, so XInput covers Xbox pads and DualSense through Steam Input/DS4Windows). Slots 0–3 polled each frame in `OnUpdate`; buttons/DPad/LB/RB/Start/Back/L3/R3 + digital stick directions (deadzone 7849) + digital triggers (threshold 30) become 24 new virtual codes in a separate range `0x1000+` (`ParkourPadCode`), so legacy keyboard INI values keep their meaning. Free Jump, Snap Direction, Parkour Mode toggle and every override can be bound to pad codes; rebind capture accepts pad buttons; UI shows connection slot or "XInput unavailable". |
| MMB fix | Rebind capture previously skipped all of `VK_LBUTTON..VK_MBUTTON`; it now skips only left/right buttons, so **middle mouse** and the X buttons are bindable in overrides (MBUTTON also works as a Free Jump/Snap/Parkour key). |
| Validation | clang++ `-fsyntax-only` (llvm-mingw, c++17, project include paths) passes for `FreeJumpPlugin.cpp` and `main.cpp` with zero errors/warnings beyond the pre-existing `-Winvalid-offsetof` notes in `AvailableParkourAction.h`. Hook-site bytes cross-checked against the runtime dump (`MODLOG/scripts/ACU_dumped_pe.exe`, RVA==file offset patch used for objdump). |
| Build/runtime status | ❌ NOT compiled with MSVC v142, NOT runtime-verified. Build via GitHub Actions (`build.yml`), then test in game. Plugin version → 0.7.0.0. |
| Manual acceptance | 1) Bind Parkour Mode to a key; toggling shows ON/OFF and the hook installs/removes (UI reports refusal if another plugin owns `0x14273BC7B`). 2) With defaults (LMB/RMB/Crouch disabled): attack/aim/crouch do nothing while the mode is on, WASD/parkour/movement unaffected, all return to normal on toggle-off. 3) Disable/enable other actions (Sprint, BombDrop, Reload, slots…) and confirm each behaves. 4) Hold an override key that shares a physical key with a disabled in-game action → only the override fires. 5) Connect an Xbox/DualSense (Steam Input) pad; bind Free Jump/override/Parkour Mode to pad buttons incl. stick directions/triggers; USB and Bluetooth both work; UI shows slot. 6) Rebind with MMB and confirm capture works and no self-trigger. 7) With ACU_ParkourPatches.dll present (its keymap hook at `0x14273BC75` stays installed) Parkour Mode still works and no crash. 8) Restart the game: all new keys, mode state, and disabled-action set persist. |

---

## Session 10 — Crash-at-load diagnosis: `bad_alloc` from a 3-byte `CCodeInTheMiddle` steal (2026-10-29)

| Item | Detail |
|------|--------|
| Symptom | First in-game run of the Session-9 build (Parkour Mode + gamepad): "Waiting for the plugins to finish loading" dialog, then `E06D7363` uncaught C++ exception, crash log stack entirely inside `ExamplePlugin.dll`. |
| Crash analysis | Mapped the crash-log RVAs (`+5ccec/589f3/5a7bd/19b2d/173e7/18672/1336d/16096`) to functions via the shipped DLL's `.pdata` (objdump, base `0x180000000`), then decoded the MSVC `ThrowInfo` at `0x1800ac610` (catchable types: `std::bad_alloc`, `std::exception`). Stack: `_CxxThrowException` → `_Xbad_alloc` → vector growth → `PresetScript_CCodeInTheMiddle` → `InstallHighlightCueHooks` → `ParryAssassinPlugin::LoadSettings`. |
| Root cause | `ParryAssassinPlugin.cpp` (Session 8 gold-highlight hook) called `PresetScript_CCodeInTheMiddle(0x14087FD09, 3, ...)`. The helper emits `nop(howManyBytesStolen - 5)`; with 3 stolen bytes that is `nop(SIZE_MAX-4)` (unsigned underflow), and the `nop` code element pushes ~1.8×10¹⁹ bytes into the resultant-code vector → allocation overflow → `std::bad_alloc`. Rule violated: a `CCodeInTheMiddle` steal must be **≥5 bytes and whole instructions** (also no RIP-relative/call/jmp among stolen bytes). |
| Why the first hook survived | `InstallHighlightCueHooks` installs in order 5 (0x14087FC88), then 3 (0x14087FD09) — the first call is legal and installed; the second threw. FreeJump's init (and all other plugins) had already run successfully before it, so the Session-9 features are not implicated. |
| Fix | Moved the hook to **0x14087FD0D, 7 stolen bytes** (`48 8B 83 40 04 00 00` = `mov rax,[rbx+0x440]` — verified whole-instruction boundary in the 1.5.0 dump; next instr at 0x14087FD14). Stack top at that site is identical (return address of the `call 0x14087f850` at 0x14087FD04 = 0x14087FD09), so the callback's return-address correlation behaves the same; it no longer fires on the `jne`-taken exit path (function returns immediately there; the deep hooks validate pending state by call-site return address + FX identity regardless). Verified all other active steals are ≥5 and whole instructions: 0x140CD9BED(5), 0x140CD9C95(5), 0x14087FC88(5), 0x14087F9B2(8), 0x14087F9E1(9), FreeJump parkour 0x14273BC7B(6). |
| Note | This bug was latent since Session 8 — the combined ExamplePlugin had never been run in-game before. The standalone `ParryassassinUpdated.dll` (older, before the glow hooks) masked it. |
| Status | Fix written + clang-checked (only the pre-existing `__try` SEH diagnostic remains). Rebuild + runtime retest pending. |
| Suggested hardening (needs approval, loader-shared file) | Add a guard in `AutoAssemblerKinda::PresetScript_CCodeInTheMiddle` that rejects `howManyBytesStolen < 5` loudly (clear exception at construction) instead of underflowing. |

---

## Session 13 — Parkour Mode: DS4Windows/controller support (third hook site) + acceptance-range widening + full move list + dual binding profiles (2026-10-29)

| Item | Detail |
|------|--------|
| Symptom | Session 12's gamepad fix did not work for the user's DS4Windows DualSense: Parkour Mode still failed to suppress pad-bound actions. |
| RE evidence (1.5.0 dump; `MODLOG/scripts/pad_chain2_out.txt`, `pad_poll_out.txt`, `helper_out.txt`, `c4c0_callers.txt`) | The input-update chain does NOT route most gamepads through the device-9 keymap stage that site 2 covers. After the pad stage's device check, device id != 9 **jumps from `0x14273BE68` into a hard-coded per-device-type mapper** (`0x142739E40..14273A736`), whose `switch` over the active device id (cases 0-7, 10, 0xB; DirectInput/HID layer — the game imports `DINPUT8.dll`/`HID.DLL` and only `XINPUT1_3.dll` **ordinal 4** (`XInputEnable`) with **zero** `XInputGetState` call sites) writes pad buttons **directly** into `keyStates[0..15]` (`+0x30..+0x3F`) and the sticks into the wasd/camera vectors — again far AFTER site 1. The mapper is the **only** entry point of that code (`jmp` @ `0x14273BE68` is its sole call/jmp), so a third hook there covers every device case including DS4Windows' virtual DirectInput pad. |
| Fix | Third `CCodeInTheMiddle` at `0x14273A721` — the whole rel32 `call FUN_142735e20` (vanilla `E8 FA B6 FF FF`; radial-deadzone vector helper whose only read parameter is the container in rcx) at the mapper's tail, immediately before its bool→float conversion at `0x14273A736`. 5 stolen bytes, `executeStolenBytes=false`, callback = named import `InputVectorDeadzoneApply(rcx, rdx, r8, r9)` + `ZeroDisabledActionKeyStates`. Register-safety proven by disassembly: `0x14273A71E` binds `RCX=RBX` (container), and after the call `0x14273A726/A72E` **reload R14/RSI from the stack**, so helper register clobbers cannot leak. All 3 sites share one `ParkourModeSuppressHook` holder; `IsParkourSuppressSitePatched()` asserts vanilla bytes at all three and the refusal text names them. |
| Feature: acceptance-range widening | New per-override checkbox "Expand acceptance range", persisted in the override line's new 10th field. Implementation ports FailedWallRunPlugin's mechanism (its proven only-loosen constants: heightDiff -3..12, horizDiff -3..3, distance 0..15, curve -4..2, expectedCurveMin -2; failing to keep a superset is exactly what produced the earlier Session-6 wallrunUpFromGroundFailed crash). Widening runs inside `ChooseBeforeFiltering`, i.e. after creation wrote the vanilla ranges and before the game's fitness/filtering consume them, and only touches actions whose null-safe type is in that override's action-type list. Documented limitation (inherited from the FailedWallRun investigation): actions discarded at **creation** by vanilla's fit test never reach the candidate array, so type 52's wallrun-up-from-ground specifically needs FailedWallRunPlugin's creation-time hooks; anything that does appear in the list can be widened. |
| Feature: full move dropdown | Action-type combos now list **all 86** named `EnumParkourAction` entries (generated from `EnumParkourAction.h`, `unk*` skipped) instead of the previous hand-written 24. |
| Feature: dual binding profiles | "Use controller bindings" checkbox (INI `UseControllerBindings=`) swaps every binding — Free Jump, Snap Direction, Parkour Mode, and every override — between a keyboard/mouse slot and a controller slot without rebinding anything. New INI keys `FreeJumpKeyPad=`, `SnapDirectionKeyPad=`, `ParkourModeKeyPad=` (hex, like their keyboard counterparts), plus per-override `padKeyCode` (decimal, 9th field). Rebinding writes to whichever profile is active (`ActiveToggleKey()`/`ActiveSnapDirectionKey()`/`ActiveParkourModeKey()`), so a controller layout is built once with the box checked and switched back and forth freely. **Both profiles' key states are refreshed every frame** (`UpdateKeyState` for each slot), the invariant that prevents the stale-rising-edge class of bug recorded in Session 11. |
| Backward compatibility | 8-field `Override=` lines (all existing user files) still load: `padKeyCode` defaults to the keyboard key, `expandRanges` off. Old `MouseLockEnabled=` lines still parse and are ignored. Unbound controller slots (0) show as `(unbound!)` in the UI and never fire. |
| Validation | `clang++ -fsyntax-only -std=c++20`: `FreeJumpPlugin.cpp` 0 errors / 3 pre-existing header warnings. Standalone INI harness reproducing the exact parser: legacy 8-field line (key mirrored, expand off), new 10-field line, malformed pad-key rejection, and serialize→parse round-trip all pass. Hook-site bytes read from the dump; mapper uniqueness verified by a full-image direct call/jmp scan. |
| Build/runtime status | ❌ NOT compiled with MSVC v142, NOT runtime-verified. Push → GitHub Actions `build.yml` → `ExamplePlugin.dll`. Plugin version → 0.10.0.0. |
| Manual acceptance | 1) Tick "Use controller bindings", rebind Parkour Mode / Free Jump / each override on the controller (DS4Windows surfaces them as pad codes `0x1000+`), untick, rebind the same on keyboard/mouse, then toggle the checkbox repeatedly — every feature must follow the active profile with no stale triggers. 2) Parkour Mode with a pad connected and DS4Windows active: attack/aim/crouch suppressed, restored on toggle. 3) No-pad session: KBM behaviour identical to Session 12 (sites 2 and 3 never run). 4) Per-override "Expand acceptance range": enable for a move (e.g. `offTheWall_fromWallToGroundReleaseOrEject`) and confirm it is reachable from noticeably farther/odd-angled positions; disable and it must return to vanilla reach. 5) Confirm widening never happens while the override is off. 6) Every named move is selectable in the dropdown. 7) Legacy INI without the new keys loads unchanged. |

---

## Session 12 — Parkour Mode for gamepads + mouse-lock feature removed (2026-10-29)

| Item | Detail |
|------|--------|
| Symptom | Parkour Mode suppression worked for keyboard/mouse but pad-bound actions (attack/aim/crouch on controller buttons) still fired while the mode was ON. |
| RE evidence (1.5.0 dump, `MODLOG/scripts/pad_input_out.txt`, `pad_chain_out.txt`, `c4c0_out.txt`) | The frame input-update chain (vfunc slot 0x0C of the keymap owner, entered at `0x14273BAC0`) is a chain of small functions: KBM fill `0x14273B2B0` → ACUFixes wheel hook `0x14273BC75` → [our site 1 `0x14273BC7B`] → mouse-vector `0x14273C190` → per-action bool→float conversion (+0x270 block) → `0x14273c580`/`0x142721500` → … → `0x14273BE10`. That last stage is the **gamepad stage**: it only proceeds when the active input device is the pad (`cmp dword ptr [InputContainerBig+0x830],9`; otherwise it jumps to `0x142739E40` and the pad section never runs). `FUN_14273BE6D` polls the pad into `InputContainerBig+0x864` and calls fill `FUN_14273b390(RCX=container, RDX=+0x864 pad state, R8=pad keymap)`, which re-writes **all 32** `keyStates_thisFrame` entries (type at keymap entry+4: 1 = pad button code read at padState[code+0x30], 2 = padState u32@0x20 == code, else 0) — AFTER site 1, so pad presses resurrected suppressed actions. The KBM fill (`0x14273B2B0`, type at entry+0 against `isPressed_byScancode`/mouse buttons) writes 0 for non-KBM types — the pad path never flows through it. The pad stage then calls `0x14273C4C0` (pad analog of the mouse-vector update; plain computation from keymap axis entries +0x4280..+0x510C and the pad buffer — no return-address/stack tricks) and repeats the bool→float conversion overwriting the same +0x270 floats. XInput note: ACU 1.5.0 imports only `XINPUT1_3.dll` ordinal 4 (XInputEnable) with zero direct call sites — the pad state arrives through the game's own device layer, which is why hooking the game's fill (not XInput) is the right seam. |
| Fix | Second `CCodeInTheMiddle` in the same `ParkourModeSuppressHook` holder at `0x14273BED2` — the whole rel32 `call 0x14273C4C0` (vanilla bytes `E8 E9 05 00 00`, dump-verified; 5 stolen bytes). A rel32 call cannot be re-executed verbatim from the trampoline, so `executeStolenBytes=false` and the callback (a) reproduces the call via the new named import `PadActionAxisAndVectorUpdate` with the saved registers (rcx=container, rdx=+0x864 pad state, r8=pad keymap — all set by the three preceding instructions and consumed identically; the code resuming at 0x14273BED7 reloads every register before use, and fxsave/fxrstor cover XMM), then (b) zeroes the disabled `keyStates_thisFrame` entries so the following bool→float conversion sees zeros. Shared `ZeroDisabledActionKeyStates(params)` helper now backs both callbacks. `IsParkourSuppressSitePatched()` asserts vanilla bytes at BOTH sites before install (refusal reported in UI with both addresses). When the pad is not the active device the pad stage is skipped and site 2 never executes — zero KBM impact. |
| Mouse-lock removal | The "Disable Left/Right Mouse Buttons (sword/aim)" checkbox and the entire `AssassinAbilitySet` ability-byte feature behind it are removed (UI checkbox, `ApplyMouseLock`/`SaveCurrentFlags`/`RestoreFlags`/`GetActiveAbilitySet`, `m_MouseLockEnabled`/`m_LastMouseLockEnabled`/`m_SavedFlags`/`m_FlagsSaved`, the `MouseLockEnabled=` INI parse + save, and the `PlayerProgressionManager.h`/`AssassinAbilitySet.h` includes). Rationale: its byte offsets (+0x22/+0x29/+0x2A) were never verified (AGENTS.md warning), and Parkour Mode's disabled-actions list (LMB/RMB default-on, now working for KBM and pad) supersedes it. Legacy INI files with `MouseLockEnabled=` are parsed without error and the key is ignored; it is no longer written. |
| Version | Plugin → 0.9.0.0. |
| Validation | `clang++ -fsyntax-only -std=c++20` (llvm-mingw, Release\|x64 include paths): `FreeJumpPlugin.cpp` 0 errors / 4 pre-existing header warnings; `main.cpp` clean. No `MouseLock`/`AssassinAbilitySet`/`PlayerProgressionManager` references remain. `.pdata`-derived function boundaries + Ghidra headless decompiles used for every new address; site bytes read directly from the dump. |
| Build/runtime status | ❌ NOT compiled with MSVC v142, NOT runtime-verified. Push → GitHub Actions `build.yml` → `ExamplePlugin.dll`. |
| Manual acceptance | 1) With a controller as the active device and Parkour Mode ON (defaults: LMB attack, RMB aim, Crouch disabled): pad attack/aim/crouch do nothing; movement/parkour unaffected; toggle OFF restores everything. 2) Disable other actions (Sprint, Reload, bomb slots…) on pad and confirm each. 3) KBM regression: with no pad connected, Parkour Mode behaves exactly as in Session 11 (site 2 never runs). 4) Pad connected but KBM active: KBM suppression still works. 5) The mouse-lock checkbox is gone from the UI; existing `FreeJumpPlugin.ini` still loads (unknown key skipped) and no longer gains a `MouseLockEnabled=` line. 6) UI shows the two-site refusal text if another plugin owns either site. 7) Pad overrides bound via the plugin's own XInput codes still fire while Parkour Mode suppresses the game's pad bindings for those actions. |

---

## Session 11 — Parkour Mode toggle-loop fix + disabled-actions UI crash fix + LMB/RMB binding + FreeJump-only wiring (2026-10-29)

| Item | Detail |
|------|--------|
| Symptom 1 | Toggling Parkour Mode flipped it ON/OFF every frame; each flip rewrote `FreeJumpPlugin.ini` and installed/removed the 0x14273BC7B suppression hook at frame rate until the machine became unresponsive. |
| Root cause 1 | `OnUpdate()` refreshed rising-edge state for `m_ToggleKey`, `m_SnapDirectionKey` and every override key — but **not** `m_ParkourModeKey`. That code was only ever updated inside `UpdateAllKeyStates()`, which runs solely while a rebind-wait is pending. After the first rebind, `m_KeyPressedThisFrame[m_ParkourModeKey]` stayed stale-`true` forever (nothing rewrites it on normal frames), `m_JustCapturedKey` resets to 0 each frame, so `WasKeyPressed(m_ParkourModeKey)` fired on every frame → infinite toggle loop. Rule recorded: **every bindable key must get a per-frame `UpdateKeyState()` call**. |
| Fix 1 | `UpdateKeyState(m_ParkourModeKey)` added to the per-frame update list in `OnUpdate()`. The toggle is now a true rising edge: one flip per press, `SaveSettings()`/hook install only on real state changes. |
| Symptom 2 | Opening the "Actions disabled while Parkour Mode is on" tree crashed the game instantly. |
| Root cause 2 | `ActionKeyName()` indexes a `names[kActionKeyCount]` (=32) array that has only 25 initializers; entries 25–31 are `nullptr`, and the UI's `!*ActionKeyName(actionIdx)` guard **dereferenced the null pointer** before the skip could happen → access violation at 0 the moment the tree opened. |
| Fix 2 | `ActionKeyName()` now returns `""` for null slots (null-safe); unnamed indices 25–31 are skipped by the existing empty-name check. |
| Feature 3 | LMB (0x01) and RMB (0x02) are now bindable everywhere (Free Jump, Snap Direction, Parkour Mode, overrides). Two guards make this safe: (a) rebind capture uses a release latch — `m_MouseButtonsSettledSinceRebindStart` (armed by detecting a rebind-wait transition in `OnUpdate`) requires LMB **and** RMB to be seen released once before a mouse-button press is accepted, so the click that opens a rebind can never capture itself; (b) `MouseBindWouldHitUi()` suppresses mouse-button toggles/holds while the loader menu is open and ImGui reports `WantCaptureMouse`, so menu clicks do not trigger in-game actions. Keyboard/pad bindings are unaffected by both guards. |
| Exclusivity | `main.cpp` and `ExamplePlugin.vcxproj` again reference **only** FreeJumpPlugin (AGENTS.md contract restored): ParryAssassinPlugin, AllowSmokeAssassinatePlugin and `main.parryassassin.cpp` are removed from the build and demoted to inactive examples (sources kept). The `ParryAssassinOnly` vcxproj switch/property group is gone. `build.yml` already builds only `ExamplePlugin.vcxproj` and uploads only `ExamplePlugin.dll` — unchanged, artifact is now FreeJump-only by construction. Plugin version → 0.8.0.0. |
| CI note | `build-parryassassin.yml` is now stale: if triggered (its `paths:` filter includes `ExamplePlugin.vcxproj`) it will build the FreeJump DLL but then fail to upload a nonexistent `ParryAssassinPlugin.dll` artifact. It does not affect `build.yml`'s green run. Recommend deleting it (awaiting user approval — not deleted in this session). |
| Validation | `clang++ -fsyntax-only -std=c++20` (llvm-mingw, exact Release\|x64 include paths): `FreeJumpPlugin.cpp` 0 errors / 6 pre-existing header warnings (`-Winvalid-offsetof` in AvailableParkourAction.h, unknown pragmas in vmath.h); `main.cpp` 0 errors / 0 warnings. `ExamplePlugin.vcxproj` parses as XML; no `ParryAssassinOnly`/other-plugin references remain in it. |
| Build/runtime status | ❌ NOT compiled with MSVC v142, NOT runtime-verified. Push → GitHub Actions `build.yml` → install `ExamplePlugin.dll` into `ACUFixes\plugins`. |
| Manual acceptance | 1) Rebind the Parkour Mode key, then toggle it: exactly one ON/OFF flip per press, no flicker, no disk thrash; INI `ParkourModeOn=` matches the UI after each press. 2) Open the disabled-actions tree: lists all named actions without crashing; toggling checkboxes + Restore defaults persists across restart. 3) Rebind Parkour Mode / Free Jump / an override to LMB: the click that starts the rebind is not captured; a deliberate LMB press after releasing it is. 4) With Parkour Mode ON and LMB bound to an override: LMB fires the override in gameplay, and clicking loader-menu controls neither toggles modes nor triggers holds. 5) Confirm the INI still round-trips old keyboard/pad bindings (no format change). 6) Verify the DLL contains only FreeJump behavior (no ParryAssassin UI entries in the loader menu). |
