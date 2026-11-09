# MODLOG — Widening a parkour move's acceptance angle (sidehop, ACU 1.5.0)
# Status: mechanism understood and playtested; documented for reuse in future parkour plugins.
# Scope: ACU 1.5.0 / ACUFixes-0.9.3 address set. Do NOT copy these addresses to 1.5.1 (see §9).

## 0. Why this file exists

`Sidehop360Plugin_150` tried to give the sidehop "360-degree execution". The **end goal was not
reached**: the player still cannot choose where the sidehop targets. But the **angle-widen mode does
something the player likes and wants preserved**: the sidehop accepts far more targets and can be
executed in far more situations.

Playtest quote (latest build, v0.3.1, Mode 1 "Angle widen"):

> "The side hop accepts way more targets or can be executed in more context, but … I still can't
> have omnidirectional control of where the side hop targets."

This file records what that mode actually does to the move, why it produces acceptance and not
aiming, and how to reproduce it safely in another plugin.

## 1. The one-sentence version

The sidehop's acceptance gate is made of **numbers written per-parkour-mode inside the action's
set-ranges function**; overriding them *after* that function has run (at the check that consumes
them) makes the move accept targets it would otherwise reject. That changes **which existing
candidates survive**, never **which candidates get created** — so it widens opportunity, not aim.

## 2. Three-stage pipeline, and where the widen sits

A parkour move exists in three stages. The angle widen only touches stage 2.

| Stage | Function | What it does | Touched by this mechanism? |
|-------|----------|--------------|----------------------------|
| 1. Creation | `climbFacade` scan call site `0x140135230` → `CreateParkourActionForParkourPointIfFits` (`0x1401D1CF0`) | Probes geometry near the wall and **creates** `AvailableParkourAction` candidates with concrete src/dest anchors | No. This is why aiming is impossible from here (see §6) |
| 2. Filtering | `InitialTestIfActionFits` `0x1401B4900` (fancy slot 0x4B): `0x64` set-ranges → `0x49` classifier `0x140147870` → **`0x4A` `0x140149CC0` = only rejection point** → `0x69` fitness `0x1401AA190` → `0x63` | Accepts/rejects candidates; consumes all the range numbers | **Yes** — both hooks live in this stage |
| 3. Ranking | GPH sort&select `0x140133B00` (owned by ACUFixes.dll / Freeform 2.0 as a function-start replacement) | Picks the winner | No |

A candidate that never exists cannot be accepted, and a candidate that is accepted still competes
with the others in stage 3. Both facts matter for reuse (§6).

## 3. The numbers, and where they are written

All offsets are `AvailableParkourAction` (see `Sidehop360Plugin_150/src/ParkourDebugging/AvailableParkourAction.h`).
`rcx` is the action in both hooks.

| Field | Offset | Vanilla value (sidehop) | Written by |
|-------|--------|------------------------|-----------|
| `minCosAngle` (the ±45° cone) | `+0x1E8` | `0x3F3504F3` = cos 45° | set-ranges `0x140148549` (`C7 81 E8 01 00 00 F3 04 35 3F`) |
| `Side_min` / `Side_max` (expected hop length) | `+0x238` / `+0x23C` | 2.0 / 3.0 m | set-ranges prologue, `mov [rcx+0x238], 0x40000000` / `[rcx+0x23C], 0x40400000` |
| `heightDifferenceMin` / `heightDifferenceMax` | `+0x1D0` / `+0x1D4` | per-mode, e.g. mode 0: `-1.0` / `+2.5` (**absolute** bounds) | per-mode jump table at the **END** of `0x140148500` |
| `horizontalDifferenceMin` / `Max` | `+0x1D8` / `+0x1DC` | 0.25 / 12.0 (**absolute**) | set-ranges header |
| `distanceMin` / `distanceMax` | `+0x1E0` / `+0x1E4` | 0.25 / 17.0 (**absolute**) | set-ranges header |
| `epsilonForDistanceRange` | `+0x1FC` | small tolerance added to every window | set-ranges |
| `epsilonForDotProd` | `+0x200` | tolerance subtracted from the cone (`minCosAngle - epsilon` vs `+0xCC`) | set-ranges |
| `directionDestFacingOut` | `+0x40` | wall's outward normal (XY, normalized) | creation, per candidate |

Checks in `QuickCheckAcceptableDistanceRanges` (`0x1401B5890`, fancy slot 0x70):

```
heightDifference          (+0xC0) in [ +0x1D0 - e , +0x1D4 + e ]
horizontalDifference      (+0xC4) in [ +0x1D8 - e , +0x1DC + e ]
distance                  (+0xC8) in [ +0x1E0 - e , +0x1E4 + e ]
cone:  +0xCC  vs  (minCosAngle - +0x200)
then 0x1401CBCD0, a +0x1F4 / +0xD4 check, and fancy slot 0x71
```

**`+0x1D4/+0x1DC/+0x1E4` are ABSOLUTE upper bounds, not offsets from their mins.**
Confirmed from the comparison order in the disassembly: `min - e <= value` and
`value <= e + max`. An earlier revision of this note (and of Sidehop360's comments) claimed they were
offsets and that the real ceiling was `min + max`; that was wrong and made Sidehop360's Hook C write
`max - min` into `+0x1D4`, so its effective ceiling was `min + max`. The behaviour was still
"permissive enough", which is why the plugin appeared to work, but the slider did not mean what the
UI said.

## 4. The two seams (verified bytes)

Dump-verified this session from `MODLOG/scripts/ACU_dumped.bin` (file offset == RVA, base
`0x140000000`):

| Site | Bytes | Meaning |
|------|-------|---------|
| `0x140148553` (Hook A, 10 stolen bytes) | `C7 81 EC 01 00 00 00 00 80 3F` | `mov dword ptr [rcx+0x1EC], 0x3F800000` — the instruction immediately **after** the cone write, so a callback here sees the vanilla cone value and can override it before the fit chain reads it |
| `0x1401B5890` (Hook C, 9 stolen bytes) | `40 53 48 83 EC 20 48 8B D9` | `push rbx; sub rsp,0x20; mov rbx,rcx` — entry of the check that consumes the ranges; the only place a `+0x1D0` override survives |

`0x1401B5890` is a *shared* impl (it can be reached from several action types' fancy vtables), which
is discussed in §7. The nudge site `0x140135230` currently holds `E9 EB AE 90 FF` + NOPs in the dump —
that is **another plugin's hook baked into the dump** (Freeform 2.0), not vanilla.

Ordering rules that make this work, in priority order:

1. **Hook C must re-apply the height window.** The per-mode jump table rewrites `+0x1D0/+0x1D4`
   *after* Hook A's site (inside the same function, reached by a jump table). A Hook A override of
   the height window is always clobbered — this was the bug that made v0.1 and v0.2 look inert.
2. Hook A must sit *after* the vanilla cone write and *before* the first consumer.
3. The callback runs, *then* the stolen bytes, then it jumps back — so `[rcx+0x1EC]` is written by
   vanilla after the callback. Do not rely on having "won" that field (we only use the seam to gain
   control flow, then write `+0x1E8` directly).

## 5. What the widen actually does in-game (and the evidence chain)

Three builds, three tests, same wall. This is the causal evidence for "acceptance, not aim":

| Build | What was overridden | Player-visible result |
|-------|--------------------|----------------------|
| v0.1 | `minCosAngle` only (`-1` = any direction) | **No behaviour change at all**, with 5550 overrides applied and vanilla `0.7071` confirmed read each time |
| v0.2 | + expected hop range (`Side_min/max`) and candidate redirect | Still no change in practice (redirect never produced a usable target) |
| v0.3 / v0.3.1 | + **allowed windows** at `0x1401B5890` (height `-10..+10`, horizontal to 30 m, distance to 40 m) | **Acceptance widens noticeably** — "accepts way more targets / executable in more contexts" |

Conclusions to reuse:

- **The cone is not the gate.** `+0x1E8` is consumed inside the check (the `+0xCC` dot-product
  comparison), not by the scanner, so widening it can only let *already-created* candidates pass.
  A full `.text` sweep found no other readers in the parkour region.
- **The allowed windows are the load-bearing part.** Height, horizontal and distance windows decide
  whether a concrete candidate survives; widening those is what the player feels.
- Widen alone still cannot move the aim: candidates are lateral probes. When *every* candidate
  passes, the same moves fire in more contexts — the observed "more targets" — but the chosen
  direction is still whatever geometry the scanner produced.

## 6. Widen vs. aim — what to use for which goal

| Goal | Widen (§3-5) | Redirect | Nudge (creation) |
|------|--------------|----------|------------------|
| Make a move fire more often / in more places | ✅ the right tool | ✗ | ~ depends on the probe point |
| Change which direction an existing move goes | ✗ | ⚠ tried, unproven (see below) | ✗ |
| Create a move toward an arbitrary bearing | ✗ (no candidate) | ⚠ | ✅ the only mechanism that adds a candidate |

Redirect (v0.2/v0.3, mode 2) rewrote `locationAnchorDest`, `dir80`, `directionSrcToDest`, both
`directionSrcToDestXYPlane*` variants, `horizontalDifference` and `distance` — the full set the
`MutateIntoSidehop` pattern needs to stay internally coherent — and still produced no usable result.
Either `0x140149CC0` (the SIMD geometry test, the only rejection point) or the object-type whitelist
(`0x140148920`, slot 0x72) rejects a target in empty air. **Unproven which**; retry only with a
telemetry hook *inside* `0x140149CC0` to see the rejection reason.

If the real goal is "cross an alley to a roof", the sidehop type may simply be wrong; session 22
noted `wallEjectToHang` (type 17) as the plausible move for that geometry.

## 7. Reuse recipe (copy this order)

1. **Confirm the exe version.** Every address here is ACU **1.5.0**. `CreateParkourActionForParkourPointIfFits`
   is `0x1401D1CF0` on 1.5.0 and `0x1401D1260` on 1.5.1 (a real bug in `FreeJumpPlugin_151` — it
   ships the 1.5.1 address in a 1.5.0 build). Re-derive per version before committing.
2. **Find the move's fancy vtable** (`AvailableParkourAction::fancyVTable` at `+0x08`), slot `0x64`
   for set-ranges and slot `0x70` for the range check. Prefer **swapping the vtable slot** (see
   `FancyVTableSwapPlugin`) over hooking `0x1401B5890`'s body if isolation matters — the check impl
   may be shared by several action types, so a body hook leaks permissive ranges to them while held.
3. **Seam A** on the instruction after the cone write in that move's set-ranges impl; override
   `minCosAngle` and `Side_*` there, and cache the wall normal from `+0x40` if you need it.
4. **Seam C** at the range check's entry (`push rbx; sub rsp,0x20; mov rbx,rcx` — 9 position-independent
   bytes); write `heightDifferenceMin/Max`, `horizontalDifferenceMin/Max`, `distanceMin/Max` as
   absolute bounds.
5. **Gate it and prove ordering.** Keep the whole thing behind a hold-key during testing, and log the
   *vanilla* value you read (`m_TelLastMinCosAngleSeen`) — that is what distinguishes "hook didn't
   run" from "hook ran but nothing consumes it". A counter is not evidence of effect; only gameplay
   is.
6. **Do not expect aim.** If you need direction, add a creation-stage candidate (nudge pattern,
   §6) rather than rewriting an existing one.

Minimal skeleton (full version: `Sidehop360Plugin_150/src/Sidehop360Plugin.cpp`):

```cpp
// Hook A: returns true only while the hotkey is held.
void WidenCave(AllRegisters* p) {
    if (!ok()) return;
    auto* a = (AvailableParkourAction*)p->rcx_;
    cacheWallNormal(*a);                    // +0x40, XY normalized
    a->minCosAngle = -1.0f;                 // 360-degree acceptance for existing candidates
    a->Side_min = 0.5f; a->Side_max = 8.0f; // expected hop length
}
struct HookA : AutoAssemblerCodeHolder_Base {
    HookA() { PresetScript_CCodeInTheMiddle(0x140148553, 10, WidenCave,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true); }
};

// Hook C: the windows that actually decide acceptance; bounds are ABSOLUTE.
void RangesCave(AllRegisters* p) {
    if (!ok()) return;
    auto* a = (AvailableParkourAction*)p->rcx_;
    a->heightDifferenceMin     = -10.0f;
    a->heightDifferenceMax     =  10.0f;             // absolute ceiling, not an offset
    a->horizontalDifferenceMin = 0.05f;  a->horizontalDifferenceMax = 30.0f;
    a->distanceMin             = 0.05f;  a->distanceMax             = 40.0f;
}
struct HookC : AutoAssemblerCodeHolder_Base {
    HookC() { PresetScript_CCodeInTheMiddle(0x1401B5890, 9, RangesCave,
                RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true); }
};
```

Hook-site ownership: check the first bytes before installing (`E9` / `FF 25` / `48 B8` = someone
else's cave). `0x1401B5890` and `0x140148553` are unowned on the test install; `0x140135230` is
Freeform 2.0's, and ACUFixes' "Parkour helpers" toggle owns it on other installs.

## 8. Known unknowns / next steps if someone resumes this

- Which check actually rejects a redirected target: instrument `0x140149CC0` (§6).
- Whether the shared `0x1401B5890` impl contributes to the "more targets" feel — i.e. how much of
  the effect is other action types getting permissive windows while held.
- The nudge (mode 3) has never been observed creating an extra sidehop: `0x140135230` was owned by
  Freeform 2.0 on every test. It is the only path to a chosen direction; disable Freeform 2.0 to test.
- Whether `wallEjectToHang` (type 17) or the beam/ledge moves are better bases for an aimed
  cross-gap traversal.

## 9. Version / evidence index

- Code: `Sidehop360Plugin_150/src/Sidehop360Plugin.cpp` (+ `.h` for the mode/version comments).
- Field layout: `Sidehop360Plugin_150/src/ParkourDebugging/AvailableParkourAction.h` (1.5.0-correct copy).
- Session log with all test results: `ExamplePlugin/src/PROGRESS.md` sessions 21-25.
- Dump + sweep tooling: `MODLOG/scripts/ACU_dumped.bin`, `MODLOG/scripts/acu_sweep.py`.
  **Caveat:** the dump is contaminated at `0x140135230` by an old Freeform 2.0 hook baked in at
  capture time; treat that one site's bytes as non-vanilla.
- Sibling mechanism doc (same style): `MODLOG/breakfall-gate.md`.

## 10. Worked example: swing-to-swing (type 49), and why a table-slot swap beats a code cave

`SwingToSwingWidenPlugin_150` applies this mechanism to `swing_31_swingToSwing_mb` (enum 49). Findings
that matter for the next reuse:

| Fact | Value |
|------|-------|
| Fancy vtable of type 49 | `0x143A77E90` (in `.data`, writable — no code patching needed) |
| Its `GetEnumParkourAction` (slot 0x45) | `0x1401B59F0` = `mov eax,0x31; ret` |
| Its set-ranges (slot 0x64) | `0x1401B60A0`, **type-exclusive** |
| Its range check (slot 0x70) | `0x1401B5890` — the **shared** check, used by 118 action types |
| Vanilla cone | mode 1 = cos30 (0.866), mode 2 = cos45 (0.7071), mode 3 = cos15 (0.966); mode 0/4+ never writes it |
| Vanilla height window | mode 1 `-2..+1`, mode 2 `-1..0`, mode 3 `-12..0` |

Because the check impl is shared, a byte hook there would leak to 118 types and would also collide with
Sidehop360's Hook C. The type's own set-ranges is exclusive but splits into per-mode branches with no
single post-write seam. The plugin therefore **repoints just slot 0x70 in the type-49 table** to a thunk
that applies the overrides and then calls `0x1401B5890`. Consequences:

- **No game code bytes are modified**, so it coexists with Freeform 2.0 (which owns `0x140135230` /
  `0x140133B00`) and with Sidehop360 (whose cave at `0x1401B5890` is still executed, because the thunk
  calls that address).
- Only type 49 candidates are affected; other moves keep vanilla acceptance even while the hotkey is held.
- Installation is a single aligned 8-byte pointer write, restored when the mode is switched off; no
  `VirtualProtect` is required (`.data` is RW) but one is done anyway so a future layout change cannot
  crash.
- Ordering: the same caveat as §5 applies — this widens acceptance of candidates that already exist, it
  does not make the swing choose a direction the scanner never probed.

This is the pattern to prefer for any move that **shares** `0x1401B5890`: swap its table slot instead of
hooking shared code. Use a byte cave only when the function you need to influence is type-exclusive.

Other swing moves that use the same shared check, widened the same way
(`SwingToWallWidenPlugin_150`, hotkey F10):

| Move | Enum | Fancy vtable(s) | Its set-ranges | Vanilla cone |
|------|------|-----------------|----------------|--------------|
| swing to front wall | 46 | `0x1439E6120`, `0x143A0B080` (both `0x140145C30`) and `0x143A66D20` (`0x1401A87D0`) | two different impls | cos15 prologue, then cos45/cos30/cos15 per mode |
| swing to sidewall (incl. spindescent) | 47 | `0x143A3E170` | `0x140190DC0` | cos75 (`0x3E8483EE`) |

Two lessons from the type-46 case: **one move can own several fancy vtables** (different creation
contexts) — a slot swap must cover all of them or the move will still be rejected in the contexts you
did not patch; and the tables of one move can even use **different set-ranges impls**, so the install
check must validate per table, not per move.

## 11. Creation-stage aim (the remaining lever for 360° targeting)

Widen and redirect both act on candidates that already exist. The only way to choose a direction is to
intervene where the candidate is **built**. Two facts discovered while looking for that seam:

1. The `climbFacade` scan is not a plain function — it is a **tester object**
   (`ParkourSystem_WhileOnWallSystem::climbFacade`, `+0x1F0`) with its own *fancy vtable*. ACUFixes'
   `Request_Spindescent.cpp` swaps that tester's table between
   `0x1439E08D0` (default) and `0x1439FA1A0` ("baseclass that has sidehop-to-swing") to add/remove
   sidehop-related scanner behaviours. That is why the helper that actually creates the sidehop has no
   static callers: it is dispatched through the tester's table.
2. `CreateParkourActionForParkourPointIfFits` (`0x1401D1CF0`) is only a **wrapper**: it forwards to a
   factory (`0x1401CC990`) that is a **jump table on the action type** (`ecx-2`, 122 cases, table at
   `0x1401CF500`); each case allocates the type's object (0x290, 0x330, ...) and runs its constructor
   with the probe point. The wrapper is called from **89 sites** and is **not owned by Freeform 2.0**
   (which hooks `0x140135230` / `0x140133B00`), so it is a free seam.

`Sidehop360Plugin` mode 4 "Aim (creation)" (v0.4.1) uses that seam. The first attempt hooked
`0x1401D1CF0` and **failed in the field**: live inspection of the running game showed the wrapper
starts `FF 25 00 00 00 00` + `0x7FFBDBC6AB90`, i.e. it is detoured by **`FreeFormWallrun.dll+0xAB90`**
(not Freeform 2.0). That plugin reimplements the wrapper instead of trampolining it — its image
contains no call to the real factory `0x1401CC990` and none to the ctor — so the wrapper is both
unusable and owned.

The working seam is the **type-39 constructor `0x1401584B0`**: vanilla, exactly one caller (the
type-39 case of the factory at `0x1401CDD4F`), arguments rcx = new action, rdx = ctx, r8 = probe
point, r9 = movement. Every creation path must pass through the factory's type-39 case, so this hook
also catches FreeForm's cave-based trampoline. Hook D steals 8 bytes
(`48 8B C4` = `mov rax,rsp`, `48 89 58 08` = `mov [rax+8],rbx`), copies the point, shifts its XY by
`AimDistance` along the movement input (r9), and repoints r8 at the copy. The vanilla ctor builds the
aimed action; the scan appends it as usual. Both pointer reads go through `ReadProcessMemory`, so a
bad argument cannot crash the game.

Telemetry is designed to localise the next failure instead of guessing:

| Signal | Meaning |
|--------|---------|
| `Sidehop constructions seen` | every type-39 ctor call (hook is live) |
| `Probe points aimed` | the substitution actually happened |
| `Aimed candidates reaching the sidehop check` | Hook C, filtered to the sidehop's fancy vtable `0x143A13130` |

The third counter must be filtered **by vtable**, not by the cave: `0x1401B5890` is shared by 118
action types, so the unfiltered counter reads thousands of unrelated overrides and is useless for
diagnosis (that is exactly what the first in-game screenshot showed).

If the third counter stays at 0 while the first climbs, the point is rejected **inside** the type-39
ctor (before any range gate) — the next lever is that ctor body, or the tester table swap in fact 1.
If it climbs but the hop still does not appear where aimed, the remaining gates are the fit chain
(`0x140149CC0`) and the sort (`0x140133B00`).
