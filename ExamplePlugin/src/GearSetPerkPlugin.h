#pragma once

#include <cstdint>

// ===========================================================================
// GearSetPerkPlugin - detect an equipped armor set and grant a perk.
//
// v1 scope (all three knobs are configurable):
//   - which armor set to detect            (m_SetIndex, catalog: Musketeer)
//   - how many pieces must be worn         (m_RequiredPieces, 1..5)
//   - which perk the player gains          (m_PerkTypeIndex + m_PerkAmount)
//
// Mechanism (no CE, no hooks):
//   AvatarGearManager::GetSingleton() -> six embedded AvatarGear objects
//   (the currently equipped pieces). Match each piece by its
//   uiString_gearName OasisLineID against the selected set's table. When
//   enough pieces match, locate the perk modifier object inside the first
//   matched piece's Modifiers array (identified by its ModifierDisplayName
//   OasisLineID) and write the perk amount into its value slot (+0x10).
//   All game-memory access is SEH-guarded; the write only happens when the
//   modifier fingerprint matches (wrong layout => skip + log, never corrupt).
// ===========================================================================

class GearSetPerkPlugin
{
public:
    void OnBeforeActivate();
    void OnUpdate();
    void OnImGuiRender();

    // ---- runtime knobs (loaded from config, editable in the ImGui panel) ----
    bool  m_Enabled;
    int   m_SetIndex;
    int   m_RequiredPieces; // 1..5
    int   m_PerkTypeIndex;
    int   m_PerkAmount;

    // ---- debug readout (refreshed every OnUpdate) ----
    bool        m_Debug_ManagerValid;
    bool        m_Debug_SetComplete;
    bool        m_Debug_ModifierFound;
    bool        m_Debug_WriteApplied;
    int         m_Debug_PiecesMatched;
    int         m_Debug_WriteCount;
    uint32_t    m_Debug_SlotLineIds[6];
    bool        m_Debug_SlotMatched[6];
    const char* m_Debug_LastError;

    // per-slot resolved details (AvatarGearManager embedded gears)
    uint32_t m_Debug_SlotGearType[6];
    uint32_t m_Debug_SlotSettingsLineId[6];
    char     m_Debug_SlotNames[6][64];
    char     m_Debug_SlotSettingsNames[6][64];

    // loadout path (PlayerProgressionManager -> AvatarLoadout)
    bool     m_Debug_LoadoutValid;
    int      m_Debug_LoadoutCount;          // valid non-null slots found
    int      m_Debug_LoadoutMusketeerCount; // diagnostic: resolved names containing "musketeer"
    uint32_t m_Debug_LoadoutLineIds[10];
    char     m_Debug_LoadoutNames[10][64];

private:
    void SaveConfigToFile();
};
