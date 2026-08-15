# HANDOFF — PhotoModePlugin: camera slots + tilt + follow-player controls + persistence + numpad + relative positioning

Repo: D:\ACUPluginLoader-ExamplePlugins-master
State: ALL feature work implemented and READY FOR BUILD. Working tree clean after latest changes.
NOT built, NOT pushed. No local MSVC — build path is GitHub Actions: user pushes, agent fixes compile errors from the build log.

## Files
- ExamplePlugin/src/PhotoModePlugin.h   (expanded with numpad keys, relative positioning flag)  — the plugin (target)
- ExamplePlugin/src/PhotoModePlugin.cpp (expanded with relative positioning logic)  — the plugin (target)
- ExamplePlugin/src/photosavemod.h/.cpp             — SOURCE reference snapshot (committed). Contains a NEWER camera model (always-roll, no tilt slider). Reference ONLY — its .cpp includes PhotoModePlugin.h, so it does not compile standalone. Do not edit, do not build.

## Feature inventory (all live in PhotoModePlugin now)

### 1. Saved camera poses with direct key bindings
   - struct CameraSlot { bool used; Vector3f pos; float yaw, pitch, fov, tilt; bool relativeToPlayer; }
   - CameraSlot m_Slots[9], m_ActiveSlot
   - SaveCurrentCameraSlot(): fills first free slot; if all 9 used, overwrites the active slot (or slot 0); NOW PERSISTS TO INI; respects m_SaveSlotsRelativeToPlayer flag
   - ApplyCameraSlot(i): restores pos/yaw/pitch/fov/tilt; applies relative positioning if relativeToPlayer=true
   - CycleCameraSlots(dir): wraps through used slots starting from m_ActiveSlot
   - Keys: ',' = previous slot, '.' = next slot (rising edge, VK_OEM_COMMA / VK_OEM_PERIOD), only while a mode is active
   - Panel: SAVED CAMERAS section — "Save Current Pose", "Clear All", per-slot apply buttons with * on active, per-slot Clear

### 2. **NEW: Direct slot key bindings (numeric 1-9)**
   - int m_SlotKeys[9] — array of virtual key codes, defaults to '1' through '9'
   - OnUpdate() checks each slot key and applies ApplyCameraSlot() on rising edge (only if slot is used and mode is active)
   - Panel displays the bound key for each slot, e.g., "Slot 1 [1|Num1]", "Slot 2 [2|Num2]", etc.
   - Help text: "Press 1-9, Numpad 1-9, ',' or '.' to switch poses ([REL] = relative to player)"

### 3. **NEW: Numpad camera slot switching (Numpad 1-9)**
   - int m_NumpadSlotKeys[9] — array of virtual key codes, defaults to VK_NUMPAD1 through VK_NUMPAD9
   - OnUpdate() checks each numpad slot key and applies ApplyCameraSlot() on rising edge (same as numeric keys)
   - Allows dual key binding per slot: either 1-9 OR Numpad 1-9 (or both can be used, whichever works best for user layout)
   - Panel shows both bindings: "Slot 1 [1|Num1]" means either key triggers the same slot

### 4. **NEW: Relative-to-player camera positioning**
   - bool m_SaveSlotsRelativeToPlayer — checkbox in the UI (defaults to OFF for backward compatibility)
   - When ON: camera positions are saved as OFFSET from player, allowing reuse as preset camera angles (e.g., "over shoulder" shot works on any NPC)
   - When OFF: camera positions are absolute world coordinates (legacy behavior)
   - CameraSlot now tracks relativeToPlayer flag — EACH SLOT REMEMBERS HOW IT WAS SAVED
   - SaveCurrentCameraSlot() checks m_SaveSlotsRelativeToPlayer:
     - If ON: pos = m_FreeCamPos - player->GetPosition()
     - If OFF: pos = m_FreeCamPos
   - ApplyCameraSlot() checks relativeToPlayer flag:
     - If true: m_FreeCamPos = player->GetPosition() + pos (applies offset to current player)
     - If false: m_FreeCamPos = pos (absolute world position)
   - Panel shows "[REL]" label on relative slots: "Slot 3 [3|Num3] [REL]"

### 5. Slot persistence across sessions (extended with new fields)
   - LoadSettings() now loads:
     - Slot key bindings (SlotKey1=...SlotKey9=)
     - Numpad slot key bindings (NumpadSlotKey1=...NumpadSlotKey9=)
     - SaveSlotsRelativeToPlayer option
     - All slot data (Slot1_Used=, Slot1_PosX=, ..., Slot1_RelativeToPlayer=)
   - SaveSettings() now writes all of the above
   - SaveCurrentCameraSlot() auto-persists immediately when a slot is saved
   - Clear operations (both "Clear All" and individual slot clears) persist changes immediately
   - INI format extended:
     ```
     SlotKey1=31                   # hex: '1'
     SlotKey2=32                   # hex: '2'
     ...
     NumpadSlotKey1=61             # hex: VK_NUMPAD1
     NumpadSlotKey2=62             # hex: VK_NUMPAD2
     ...
     SaveSlotsRelativeToPlayer=0   # 0=world position, 1=relative to player
     Slot1_Used=1
     Slot1_PosX=100.5
     Slot1_PosY=200.3
     Slot1_PosZ=-50.1
     Slot1_Yaw=0.785
     Slot1_Pitch=0.45
     Slot1_Fov=1.0
     Slot1_Tilt=0.0
     Slot1_RelativeToPlayer=1      # NEW: tracks how this slot was saved
     Slot2_Used=0                  # unused slot
     ...
     ```

### 6. Tilt Angle slider: range -180..180 (was -45..45)

### 7. Tilt persisted in slots: CameraSlot.tilt; SaveCurrentCameraSlot stores m_TiltAngle, ApplyCameraSlot restores it

### 8. Follow Player input lock (option m_FollowAllowMouse, INI key FollowAllowMouse=)
   - Follow Player ON: arrow keys, Q/E, mouse orbit AND FOV wheel are ALL ignored by default
   - "Allow Mouse Look (Follow)" checkbox re-enables mouse orbit + wheel ONLY; arrows/Q/E stay locked
   - Gating in UpdateFreeInput(): followLocked = m_FollowPlayer && !m_FollowAllowMouse; lookBlocked = followLocked || frozen (mouse block); movement gate = !frozen && !m_FollowPlayer

### 9. Mouse Tilt option (m_MouseTilt, INI key MouseTilt=)
   - When ON: mouse X adjusts m_TiltAngle (clamped ±180, 3x mouse sensitivity, InvertX applies); mouse Y still orbits pitch; yaw untouched
   - Panel checkbox under the Tilt Angle slider

### 10. Follow Player teleport bug FIXED
   - Root cause: toggling Follow from the panel never captured m_FollowOffset; next frame re-anchored to player pos + stale offset (0,0,0 or leftovers) -> camera jumped away from the composed shot
   - Fix: checkbox handler, when enabling Follow while m_Mode == Mode::Free, sets m_FollowOffset = m_FreeCamPos - player->GetPosition() so the camera stays exactly where it was and tracks Arno from there

## Behavior matrix (Follow Player ON)
- Default: camera tracks Arno at the setup offset; ALL camera control locked; Arno fully playable
- Allow Mouse Look ON: mouse orbit + FOV wheel work; arrows/Q/E still locked

## Behavior matrix (Save Slots Relative to Player ON)
- Save a slot: offset from Arno's position is stored (e.g., "2 units behind, 1 unit above")
- Load the slot on same Arno: camera restores to exact same relative position
- Load the slot on different NPC: camera applies same offset to that NPC's position (useful for "talking head" shots)

## Conventions / pitfalls (settled in prior sessions)
- NEVER write quaternion_mb as a roll-around-lookat quaternion (settled after 3 failed builds: any quat write tilts AND maps yaw->roll, killing left/right rotation). Current code writes quaternion_mb via BuildLevelFreeCameraQuaternion(yaw,pitch,m_TiltAngle) or BuildFreeCameraQuaternion — leave that mechanism alone unless explicitly asked.
- EOL: PhotoModePlugin files are LF (git prints a harmless "LF will be replaced by CRLF" warning); photosavemod files are CRLF.
- File tools on this host: use native D:\... paths for read_file/write_file/patch (MSYS /d/... paths get mangled by file tools; terminal git-bash resolves /d/ fine).
- Large patch()/write_file() calls time out the stream (~8K chars per call): split edits into small hunks; never run two patches to the same file in one parallel batch.
- ACU work comes as numbered Handoff contracts: obey scope literally; read-only means zero writes; report + stop with file:line citations; "Not found" over inventing.
- Build/push only when the user asks.

## Testing checklist (user-facing)

### Basic slot functionality
1. Save 3+ poses with different keys (F9 to enter photo mode, move camera, click "Save Current Pose", repeat)
2. Close the game completely and relaunch
3. Open Photo Mode: verify all saved poses are still there with the same positions/angles
4. Verify pressing 1, 2, 3 switches directly to those poses
5. Verify pressing Numpad 1, 2, 3 also switches to those poses
6. Verify ',' and '.' still cycle through slots
7. Create a new pose, save it, close/reopen game: verify it persists

### Relative positioning feature
1. Enable "Save Slots Relative to Player" checkbox
2. Position camera over Arno's shoulder (offset: 2m right, 1m back, 0.5m up)
3. Click "Save Current Pose" — slot should show "[REL]" label
4. Move Arno to a different location or switch to a different NPC
5. Load that slot: camera should be at the SAME RELATIVE POSITION to the new NPC
6. Disable "Save Slots Relative to Player" and save a new pose (world position)
7. Load both types: relative slot should follow player, world slot should stay fixed

### Key layout compatibility
1. If numpad keys don't register: verify numpad is in number mode (not NumLock OFF)
2. If 1-9 keys conflict with in-game bindings: numeric keys can be rebound in-game or via INI file editing

## Next steps
1. Build via GitHub Actions (user pushes; fix errors from the build log if any)
2. In-game verification (see Testing checklist above)
3. If numpad or numeric slot keys do not register: check system key layout; can rebind to layout-safe keys (e.g., J/K, F1-F9, or NumPad 0). Panel buttons always work regardless.
4. If relative slots don't update when player moves: verify m_SaveSlotsRelativeToPlayer is ON and slots show "[REL]" label

## Open risks
- Numeric and numpad slot keys may conflict with in-game bindings (rare, but possible) — easily rebound via INI
- Relative positioning requires player to exist; if player is nullptr, reverts to world positioning (fallback is graceful)
