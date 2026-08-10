#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// StunBladePlugin (brute-force version)
// ---------------------------------------------------------------------------
// When enabled, EVERY FRAME the player's HumanStatesHolder is checked: if
// ballisticAimingCurrentEquipmentType is the PhantomBlade (throwblade, 0x16)
// it is immediately rewritten to StunBomb (0x14). No hooks, no code caves,
// no stolen bytes, no key to hold, no crash surface. Wherever the game
// resolves a throw / projectile / quickshot, it reads the field and finds
// StunBomb, so the blade becomes a genuine stun bomb at any instance.
//
// Other throwables (smoke/cherry/poison/moneypouch, pistols, rifle...) are
// untouched - the rewrite only fires while the field is exactly PhantomBlade.
// Stun-bomb ammo is consumed (you must own/stack stun bombs).
//
// Debug readout refreshes every frame (even while disabled) so a stale/NULL
// pointer chain is visible in the ImGui section before you turn the plugin
// on. NULLs are EXPECTED in the main menu (no player entity) - read it in
// gameplay while aiming the throwblade.
// ---------------------------------------------------------------------------

class StunBladePlugin
{
public:
    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();

private:
    bool m_Enabled = true;    // master switch (single checkbox, that's all)

    // ---- debug readout (refreshed every frame, even while disabled) ----
    bool     m_Debug_HumanStatesValid       = false; // GetForPlayer() != null
    bool     m_Debug_OwnerEntityValid       = false; // holder->ownerEntity != null
    bool     m_Debug_OwnerIsPlayer          = false; // ownerEntity == ACU::GetPlayer()
    uint32_t m_Debug_CurrentEquipmentType   = 0;     // live 0x0D34 field value
};
