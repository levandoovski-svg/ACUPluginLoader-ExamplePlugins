# HANDOFF — PhotoModePlugin: camera slots + tilt + follow-player controls + persistence

Repo: D:\ACUPluginLoader-ExamplePlugins-master
State: ALL feature work implemented and READY FOR BUILD. Working tree clean after latest changes.
NOT built, NOT pushed. No local MSVC — build path is GitHub Actions: user pushes, agent fixes compile errors from the build log.

## Files
- ExamplePlugin/src/PhotoModePlugin.h   (131 lines + new slot key bindings)  — the plugin (target)
- ExamplePlugin/src/PhotoModePlugin.cpp (950+ lines)  — the plugin (target)
- ExamplePlugin/src/photosavemod.h/.cpp             — SOURCE reference snapshot (committed). Contains a NEWER camera model (always-roll, no tilt slider). Reference ONLY — its .cpp includes PhotoModePlugin.h, so it does not compile standalone. Do not edit, do not build.

## Feature inventory (all live in PhotoModePlugin now)

1. Saved camera poses (ported verbatim from photosavemod)
   - struct CameraSlot { bool used; Vector3f pos; float yaw, pitch, fov, tilt; } + CameraSlot m_Slots[9], m_ActiveSlot
   - SaveCurrentCameraSlot(): fills first free slot; if all 9 used, overwrites the active slot (or slot 0); NOW PERSISTS TO INI
   - ApplyCameraSlot(i): restores pos/yaw/pitch/fov/tilt; if Follow Player is on in Free mode, re-anchors m_FollowOffset
   - CycleCameraSlots(dir): wraps through used slots starting from m_ActiveSlot
   - Keys: ',' = previous slot, '.' = next slot (rising edge, VK_OEM_COMMA / VK_OEM_PERIOD), only while a mode is active
   - Panel: SAVED CAMERAS section — "Save Current Pose", "Clear All", per-slot apply buttons with * on active, per-slot Clear

2. **NEW: Direct slot key bindings (1-9)**
   - int m_SlotKeys[9] — array of virtual key codes, defaults to '1' through '9'
   - OnUpdate() now checks each slot key and applies ApplyCameraSlot() on rising edge (only if slot is used and mode is active)
   - Panel now displays the bound key for each slot, e.g., "Slot 1 [1]", "Slot 2 [2]", etc.
   - Help text updated: "Use 1-9 keys, ',' or '.' to switch between saved poses"

3. **NEW: Slot persistence across sessions**
   - LoadSettings() now loads slot key bindings (SlotKey1=...SlotKey9=) and all slot data (Slot1_Used=, Slot1_PosX=, etc.)
   - SaveSettings() now saves slot key bindings AND all slot data to the INI file
   - SaveCurrentCameraSlot() calls SaveSettings() to persist immediately when a slot is saved
   - Clear operations (both "Clear All" and per-slot clear buttons) now call SaveSettings()
   - INI format:
     ```
     SlotKey1=31           # hex: '1'
     SlotKey2=32           # hex: '2'
     ...
     Slot1_Used=1
     Slot1_PosX=100.5
     Slot1_PosY=200.3
     Slot1_PosZ=-50.1
     Slot1_Yaw=0.785
     Slot1_Pitch=0.45
     Slot1_Fov=1.0
     Slot1_Tilt=0.0
     Slot2_Used=1
     ...
     ```

4. Tilt Angle slider: range -180..180 (was -45..45)

5. Tilt persisted in slots: CameraSlot.tilt; SaveCurrentCameraSlot stores m_TiltAngle, ApplyCameraSlot restores it

6. Follow Player input lock (option m_FollowAllowMouse, INI key FollowAllowMouse=)
   - Follow Player ON: arrow keys, Q/E, mouse orbit AND FOV wheel are ALL ignored by default
   - "Allow Mouse Look (Follow)" checkbox re-enables mouse orbit + wheel ONLY; arrows/Q/E stay locked
   - Gating in UpdateFreeInput(): followLocked = m_FollowPlayer && !m_FollowAllowMouse; lookBlocked = followLocked || frozen (mouse block); movement gate = !frozen && !m_FollowPlayer

7. Mouse Tilt option (m_MouseTilt, INI key MouseTilt=)
   - When ON: mouse X adjusts m_TiltAngle (clamped ±180, 3x mouse sensitivity, InvertX applies); mouse Y still orbits pitch; yaw untouched
   - Panel checkbox under the Tilt Angle slider

8. Follow Player teleport bug FIXED
   - Root cause: toggling Follow from the panel never captured m_FollowOffset; next frame re-anchored to player pos + stale offset (0,0,0 or leftovers) -> camera jumped away from the composed shot
   - Fix: checkbox handler, when enabling Follow while m_Mode == Mode::Free, sets m_FollowOffset = m_FreeCamPos - player->GetPosition() so the camera stays exactly where it was and tracks Arno from there

## Behavior matrix (Follow Player ON)
- Default: camera tracks Arno at the setup offset; ALL camera control locked; Arno fully playable
- Allow Mouse Look ON: mouse orbit + FOV wheel work; arrows/Q/E still locked

## Conventions / pitfalls (settled in prior sessions)
- NEVER write quaternion_mb as a roll-around-lookat quaternion (settled after 3 failed builds: any quat write tilts AND maps yaw->roll, killing left/right rotation). Current code writes quaternion_mb via BuildLevelFreeCameraQuaternion(yaw,pitch,m_TiltAngle) or BuildFreeCameraQuaternion — leave that mechanism alone unless explicitly asked.
- EOL: PhotoModePlugin files are LF (git prints a harmless "LF will be replaced by CRLF" warning); photosavemod files are CRLF.
- File tools on this host: use native D:\... paths for read_file/write_file/patch (MSYS /d/... paths get mangled by file tools; terminal git-bash resolves /d/ fine).
- Large patch()/write_file() calls time out the stream (~8K chars per call): split edits into small hunks; never run two patches to the same file in one parallel batch.
- ACU work comes as numbered Handoff contracts: obey scope literally; read-only means zero writes; report + stop with file:line citations; "Not found" over inventing.
- Build/push only when the user asks.

## Testing checklist (user-facing)
1. Save 3+ poses with different keys (e.g., press F9, move camera, click "Save Current Pose", repeat)
2. Close the game completely and relaunch
3. Open Photo Mode: verify all saved poses are still there with the same positions/angles
4. Verify pressing 1, 2, 3 (or whatever slots were saved) switches directly to those poses
5. Verify ',' and '.' still cycle through slots
6. Create a new pose, save it, close/reopen game: verify it persists
7. Clear a slot: close/reopen game, verify it's cleared in the saved INI file

## Next steps
1. Build via GitHub Actions (user pushes; fix errors from the build log if any)
2. In-game verification (see Testing checklist above)
3. If '1'-'9' keys do not register (key-layout dependent): rebind to layout-safe keys (e.g., J/K, or NumPad 1-9 VK_NUMPAD1..VK_NUMPAD9). Current defaults work for US/Arabic/most layouts.
4. If slots don't persist: check INI file format (Documents\Assassin's Creed Unity\PhotoModePlugin.ini); verify it's being read/written correctly

## Open risks
- Numeric slot keys may collide with in-game bindings (rare, but possible)
- INI file location is Documents\Assassin's Creed Unity\PhotoModePlugin.ini — verify write permissions
