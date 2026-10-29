# SplitScreenCoopPlugin

Local couch co-op (split-screen) plugin for **Assassin's Creed Unity 1.5.0**, built on the
ACUFixes Plugin Loader 0.9.3. Goal: two players in one free-roam world, side-by-side views,
same avatar, independent cameras, fully offline.

Current status: **Phase 0 scaffold** — no gameplay hooks yet. Verified RE evidence and the
phase plan live in `../MODLOG/split-screen-coop.md`.

## Build (GitHub Actions)

- Workflow: `.github/workflows/build-splitscreen.yml` (runs on pushes touching this plugin or
  `CommonLibACU`, and on manual dispatch).
- Configuration: `Release|x64`, toolset `v142`, C++20.
- Artifact: `SplitScreenCoopPlugin/build/x64/Release/plugins/SplitScreenCoopPlugin.dll`.

## Install

1. Copy `SplitScreenCoopPlugin.dll` into the game's plugin directory:
   `Assassin's Creed Unity\ACUFixes\plugins\`
2. Start the game. No game files are modified or replaced.

## Remove

Delete `SplitScreenCoopPlugin.dll` from the plugins directory (while the game is closed).
Optionally also remove the plugin's config/log (see below).

## Configuration & logs

Created automatically on first run:

- `Documents\Assassin's Creed Unity\SplitScreenCoop.ini`
  - `Enabled` — master toggle; when `0` the plugin does nothing gameplay-related.
  - `P2ControllerIndex` — XInput controller (0..3) reserved for player 2.
  - `SplitDirection` — `1` side-by-side (P1 left, P2 right), `2` top/bottom. Reserved for Phase 4.
- `Documents\Assassin's Creed Unity\SplitScreenCoop.log` — session log (appended per run).

The same settings are editable from the plugin's ImGui window (loader menu) and saved eagerly.

## Notes for developers

- The plugin exports `ACUPluginStart` through `Common_PluginSide` (`__declspec(dllexport)`),
  same contract as `ExamplePlugin`; there is no `.def` file.
- XInput is loaded dynamically (`xinput1_4.dll` → `xinput1_3.dll` → `xinput9_1_0.dll`);
  no new link-time dependency.
- Phase 0 performs no code patching and calls no game functions.
