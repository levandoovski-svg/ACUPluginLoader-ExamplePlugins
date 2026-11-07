# FreeFormWallrun (ACU 1.5.0)

Passive freeform wallrun. While the hotkey is ACTIVE, parkour candidates are
filtered at creation:
- natural freeform 52s (`wallrunUpFromGroundFailed_mb`) are **kept alive**
  even when the vanilla `InitialTestIfActionFits` reject would discard them;
- wallrun 53 (`wallrunUpFromGround_mb`) and wallrun-to-overhang 54
  (`wallrunUpToHang`) are **discarded at creation** via the vanilla destroy
  path (identical to a native reject).

The game then picks among the surviving candidates itself — no sort&select
site ownership (so FreeJump keeps its own cheap callback path instead of its
heavy `FinalFilter1` fallback cave), no vtable conversion, no weight boost,
no force-select. On non-standable objects nothing happens — use FreeJump's
own overrider there.

**Use:** ACUPluginLoader menu → enable → hold/toggle hotkey (default F8).

**Config:** `Documents\Assassin's Creed Unity\FreeFormWallrun.ini`
(legacy `FancyVTableSwapPlugin.ini` is migrated on first load).
