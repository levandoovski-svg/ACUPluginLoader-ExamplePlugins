# DiscardMovePlugin

Lets you pick arbitrary parkour moves (the `EnumParkourAction` values) which get
**discarded at creation** — the same hook and mechanism FreeFormWallrun uses to
kill wallruns 53/54 — while a hotkey is ACTIVE.

Target game version: **ACU 1.5.0** (built against `ACUFixes 1.5.0` includes).

## How it works

Only the `ForceCreationDiscard_fnp` callback of `GenericHooksInParkourFiltering`
is used: while the hotkey is ACTIVE, any created action whose type is on your
discard list is destroyed through the vanilla destroy path (identical in shape
to a native reject), so it can never win selection. The sort&select site is left
untouched and no keep-alive override is registered.

Killing an action at creation is safe even if the game falls back to "just stand
there" — with an empty list the plugin is inert.

## UI (loader menu, ImGui)

- Enable checkbox, ACTIVE/INACTIVE status.
- Hold/Toggle mode for the hotkey, with rebind (keyboard, mouse, XInput pad).
- A dropdown listing every `EnumParkourAction`, an "Add" and per-entry "Remove".
- The discard list persists to
  `%USERPROFILE%\Documents\Assassin's Creed Unity\DiscardMovePlugin.ini`
  as `Discard=<enum name>,<enum name>,...`.

## Notes

- The discard decision runs after any keep-alive overrides from other plugins
  (e.g. FreeFormWallrun's freeform-52 rescue), so a move you list here wins.
- Prefer a hotkey that is not bound by the game and not used by FreeFormWallrun
  (default there and here is F8 — rebind one of them if you run both).
