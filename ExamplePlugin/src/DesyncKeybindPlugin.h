#pragma once

#include <cstdint>

// Hotkey-triggered "instant Animus desync" plugin.
//
// Member-plugin pattern (NOT an ACUPluginInterfaceVirtuals subclass):
// the ExamplePlugin DLL may only ever have ONE live instance of
// ACUPluginInterfaceVirtuals (Common_PluginSide's ACUPluginStart dispatches
// to whatever instance the base-class constructor registered last), so this
// class is aggregated as a member of the host ExamplePlugin class in
// main.cpp and driven from that class's virtual callbacks.
//
// Trigger logic: writes CSrvPlayerHealth::isDesynchronizationNow (offset
// 0x22C) to true, which makes the engine play the Animus desync "instant
// death" sequence. The write is SEH-guarded and POD-only, exactly like
// AutoDesyncOnAssassinatePlugin, so a stale pointer chain can never take the
// game down.
//
// Hotkey reading uses the Windows GetAsyncKeyState() API (rising-edge via
// `& 1`), NOT the game's input system -- that keeps the hotkey independent
// of the game's InputContainer singleton addresses, which are not
// guaranteed to be correct in this vendored tree. Config is persisted to
//   <Documents>\Assassin's Creed Unity\DesyncKeybindPlugin.ini

class DesyncKeybindPlugin
{
public:
    void LoadSettings();
    void SaveSettings();

    // Called every frame, whether the ImGui menu is open or not.
    void OnUpdate();
    // Called every frame while the ImGui menu section is visible.
    void OnImGuiRender();

private:
    bool m_Enabled       = true;   // hotkey armed
    int  m_KeyCode       = 0x78;   // Windows VK code, default VK_F9
    bool m_WaitingForKey = false;  // rebind capture in progress

    // Auto-desync on assassination: when any tracked assassination human
    // state is detected in the player's state tree, fire the desync write.
    bool m_AutoDesyncOnAssassination = true;  // persisted to the INI
    bool m_PrevInAssassinationState  = false; // rising-edge bookkeeping

    // Class gate: assassination is allowed for highlighted victims whose
    // EntityDescriptor_ SubDescriptorType is in m_AllowedClasses (CSV of
    // EntityDescriptorNPCSubType ids; Target=6, UniqueNPC=33 by default).
    bool m_OnlyTargetsAllowed = true;
    char m_AllowedClasses[64] = "6,33";

    // Grace window for the class gate. The highlighted-NPC pointer can
    // lag a few frames behind the assassination state entry, so the
    // verdict is deferred up to kGateGraceFrames after the edge:
    //   - victim resolves to an allowlisted class  -> allowed, no desync
    //   - victim resolves to any other class       -> desync immediately
    //   - victim never resolves within the window  -> desync at expiry
    //     (preserves the pre-gate default behavior)
    static constexpr int kGateGraceFrames = 20; // ~0.33s at 60 fps
    int  m_GateGraceRemaining = 0;
    bool m_GatePendingFire    = false;
    bool m_GateVerdictAllowed = false; // last window outcome, for debug

    // Debug readouts (refreshed every frame, even while disabled).
    bool m_Debug_CameraComponentValid = false;
    bool m_Debug_PlayerEntityValid    = false;
    bool m_Debug_BhvAssassinValid     = false;
    bool m_Debug_HealthValid          = false;
    bool m_Debug_InAssassinationState = false;
    int  m_Debug_AssassinationDetections = 0;
    bool m_Debug_VictimValid             = false;
    uint32_t m_Debug_VictimDescriptorType    = 0;
    uint32_t m_Debug_VictimSubDescriptorType = 0;
    uint32_t m_Debug_VictimExplicitProperty  = 0;
    int  m_Debug_AllowedAssassinations  = 0;
    int  m_Debug_DesyncWrites         = 0;
    bool m_Debug_GatePending            = false;
    bool m_Debug_LastTriggerSucceeded = true; // start optimistic
};