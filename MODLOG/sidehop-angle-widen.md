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
| `heightDifferenceMin` / `heightDifferenceMax` | `+0x1D0` / `+0x1D4` | per-mode, e.g. mode 0: `-1.0` / `+2.5` (**upper = min + offset = +1.5 m**) | per-mode jump table at the **END** of `0x140148500` |
| `horizontalDifferenceMin` / `Max` | `+0x1D8` / `+0x1DC` | 0.25 / 12.0 (upper = 12.25 m) | set-ranges header |
| `distanceMin` / `distanceMax` | `+0x1E0` / `+0x1E4` | 0.25 / 17.0 (upper = 17.25 m) | set-ranges header |
| `epsilonForDistanceRange` | `+0x1FC` | small tolerance added to every window | set-ranges |
| `epsilonForDotProd` | `+0x200` | tolerance subtracted from the cone (`minCosAngle - epsilon` vs `+0xCC`) | set-ranges |
| `directionDestFacingOut` | `+0x40` | wall's outward normal (XY, normalized) | creation, per candidate |

Checks in `QuickCheckAcceptableDistanceRanges` (`0x1401B5890`, fancy slot 0x70):

```
heightDifference          (+0xC0) in [ +0x1D0 - e , +0x1D0 + 0x1D4 ]
horizontalDifference      (+0xC4) in [ +0x1D8 - e , +0x1D8 + 0x1DC ]
distance                  (+0xC8) in [ +0x1E0 - e , +0x1E0 + 0x1E4 ]
cone:  +0xCC  vs  (minCosAngle - +0x200)
then 0x1401CBCD0 and a +0x1F4 / +0xD4 check
```

**`+0x1D4/+0x1DC/+0x1E4` are stored as offsets from their min, not absolute maxima.** Writing an
absolute upper bound into them silently makes the window `min + upper`; that is the #1 way to get
"widen applied, nothing changed".

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
   `min` + **offset**.
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

// Hook C: the windows that actually decide acceptance; upper bounds are OFFSETS.
void RangesCave(AllRegisters* p) {
    if (!ok()) return;
    auto* a = (AvailableParkourAction*)p->rcx_;
    a->heightDifferenceMin     = -10.0f;
    a->heightDifferenceMax     =  10.0f - (-10.0f);  // stored as offset from min
    a->horizontalDifferenceMin = 0.05f;  a->horizontalDifferenceMax = 30.0f - 0.05f;
    a->distanceMin             = 0.05f;  a->distanceMax             = 40.0f - 0.05f;
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
- Session log with all test results: `ExamplePlugin/src/PROGRESS.md` sessions 21-24.
- Dump + sweep tooling: `MODLOG/scripts/ACU_dumped.bin`, `MODLOG/scripts/acu_sweep.py`.
  **Caveat:** the dump is contaminated at `0x140135230` by an old Freeform 2.0 hook baked in at
  capture time; treat that one site's bytes as non-vanilla.
- Sibling mechanism doc (same style): `MODLOG/breakfall-gate.md`.
