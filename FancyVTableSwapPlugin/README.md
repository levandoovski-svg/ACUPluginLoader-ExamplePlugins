# FancyVTableSwapPlugin (ACU 1.5.0)

Live swap of `AvailableParkourAction::fancyVTable` (+0x8, 0x78 entries).

**How it works (ACUFixes 1.5.0):** ACUFixes `Request_Spindescent.cpp`
swaps the climb-facade tester's fancy vtable by overwriting
`*(uint64*)((uint64)obj+8)`. This plugin does the same one-pointer write,
but for any parkour move, from the official plugin seam
`GenericHooksInParkourFiltering::ChooseBeforeFiltering` — so it runs
before fitness/sort/final-filters, with no raw patch addresses of its own.

**Use:** ACUPluginLoader menu → enable → set Source / Dest → hold/toggle
hotkey (default F8). Source = vtable you want to replace, Dest = vtable
to install. The panel lists the vtables actually present this parkour
cycle (live addresses, nothing hardcoded); click one to set as Source,
`-> Dest` to set as destination. Optional type filter narrows by
`EnumParkourAction` (idx 0x45, null-slot safe).

**Config:** `Documents\Assassin's Creed Unity\FancyVTableSwapPlugin.ini`

**Warnings:**
- Only swap tables of the same 0x78-entry size. Mismatched sizes crash.
- The action's data layout is unchanged — only its functions are swapped.
- `shortcut:` single rule only; upgrade to a rule vector for multi-swap.
- `shortcut:` only `AvailableParkourAction` tables; tester/scanner/state-node tables need per-class offset/size — out of scope.
