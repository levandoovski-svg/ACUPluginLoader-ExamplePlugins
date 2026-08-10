#pragma once

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
};