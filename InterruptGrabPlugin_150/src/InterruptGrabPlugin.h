#pragma once

#include <Windows.h>
#include <Xinput.h>
#include <cstdint>
#include <string>

// Interrupt Grab (ACU 1.5.0, no ASLR; image base 0x140000000)
//
// Hold the hotkey to force the parkour action-selection driver to take the
// EPM_ParkourDown (3) branch, so a hanging/grabbing Arno runs the drop
// scanners and releases the grab. Same idea as the AC4/Mirage "Interrupt
// Grab" mods.
//
// Addresses are from the runtime dump (MODLOG/scripts/ACU_dumped.bin):
//   0x1419FFFF7  parkour action-selection driver (FUN_1419FFFF7)
//   0x141A00091  cmp r14d, 3      (r14d = EnumParkourMode for this frame)
//   0x141A00157  movzx ebx,[rbp+0x350]   <- patch point, value irrelevant
//   0x141A0015E  continuation
//
// r14d is only consumed as the parkour mode after the patch point: it is
// stored to [rsp+0x28] before each of the four scanners (0x141A3C050,
// 0x141A3CEA0, 0x141A3C4E0, 0x141A3D0A0) and to edx at the final call
// 0x141A944A0. The cave forces it to 3; the pre-patch-point logic (the
// cmp at 0x141A00091 and the r14d=4 upgrade at 0x141A00151) already ran
// with the game's own value, which is exactly the ASI behaviour.
class InterruptGrabPlugin
{
public:
    void OnBeforeActivate();   // InitStage_WhenCodePatchesAreSafeToApply
    void OnUpdate();           // every frame
    void OnImGuiRender();      // every frame while the menu is open

    bool ShouldPatch() const;  // enabled && hotkey held
    bool ApplyPatch();
    void RevertPatch();

    bool m_Enabled = true;
    int  m_Hotkey = 'C';             // VK_C, rebindable (keyboard)
    bool m_WaitingForHotkey = false;
    bool m_CaptureArmed = false;
    int  m_PadButton = XINPUT_GAMEPAD_B;  // 0 = pad binding disabled

    // Site signature check result. If false the plugin never patches.
    bool m_SupportedBuild = false;
    std::string m_StatusMessage;

    long long m_TelActivations = 0;
    bool      m_PatchApplied = false;

private:
    void LoadSettings();
    void SaveSettings();
    static std::string GetIniPath();
    bool BuildCave();
};
