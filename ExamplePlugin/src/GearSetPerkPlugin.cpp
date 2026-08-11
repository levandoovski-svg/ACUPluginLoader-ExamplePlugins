#include "pch.h"
#include "GearSetPerkPlugin.h"

#include "MyLog.h"
#include "MainConfig.h"

#include "ACU/AvatarGearManager.h"
#include "ACU/AvatarGear.h"

// ===========================================================================
// Catalogs
// ===========================================================================

struct GearSetDef
{
    const char* name;
    // [slot: Head, Chest, Forearms, Waist, Legs][rarity tier 0..3; 0 = none]
    // Values are uiString_gearName OasisLineIDs from the AnvilToolkit dump of
    // the live AvatarGearManager (791_-_AvatarGearManager_0X110741B51F.xml).
    uint32_t pieceLineIds[5][4];
};

static const GearSetDef kGearSets[] = {
    {
        "Musketeer",
        {
            { 502500, 502502, 502504, 502506 }, // Head
            { 502340, 502342, 502344, 502346 }, // Chest
            { 502420, 502422, 502424, 502426 }, // Forearms
            { 502742, 502744, 502746, 502748 }, // Waist
            { 502596, 502598, 502600, 502602 }, // Legs
        },
    },
};

struct PerkDef
{
    const char* name;
    uint32_t modifierDisplayLineId; // OasisLineID of the modifier's ModifierDisplayName
};

static const PerkDef kPerks[] = {
    { "Bullet capacity",       584386 },
    { "Smoke bomb capacity",   557196 },
    { "Stun grenade capacity", 557197 },
    { "Throwblade capacity",   557211 },
    { "Berserk dart capacity", 557212 },
    { "Poison gas capacity",   557198 },
    { "Cherry bomb capacity",  557200 },
    { "Potion capacity",       557195 },
    { "Lockpicks capacity",    584159 },
    { "Money pouch capacity",  557199 },
    { "Additional HP",         557201 },
};

static int SetCatalogCount()  { return (int)(sizeof(kGearSets) / sizeof(kGearSets[0])); }
static int PerkCatalogCount() { return (int)(sizeof(kPerks) / sizeof(kPerks[0])); }
static int ClampIndex(int v, int count) { return (count <= 0) ? 0 : (v < 0 ? 0 : (v >= count ? count - 1 : v)); }

// ===========================================================================
// AvatarGearModifier layout
// ===========================================================================
// Modifier objects are polymorphic. The AnvilToolkit dump serializes every
// modifier as: ModifierDisplayName (UIString) + UnitName (UIString) + value.
// UIString is asserted 4 bytes (UIString.h), so in memory:
//   +0x00 vtable (8 bytes, from Object)
//   +0x08 ModifierDisplayName stringID
//   +0x0C UnitName stringID
//   +0x10 value (uint32 for capacity mods, float for damage mods - same slot)
struct GearModifierLayout
{
    void*    vtable;
    uint32_t displayNameId;
    uint32_t unitNameId;
    uint32_t value;
};

// ===========================================================================
// GearSetPerkPlugin
// ===========================================================================

void GearSetPerkPlugin::OnBeforeActivate()
{
    m_Enabled        = g_Config.gearSetPerk.enabled;
    m_SetIndex       = g_Config.gearSetPerk.setIndex;
    m_RequiredPieces = g_Config.gearSetPerk.requiredPieces;
    m_PerkTypeIndex  = g_Config.gearSetPerk.perkTypeIndex;
    m_PerkAmount     = g_Config.gearSetPerk.perkAmount;

    if (m_RequiredPieces < 1) { m_RequiredPieces = 1; }
    if (m_RequiredPieces > 5) { m_RequiredPieces = 5; }

    m_Debug_ManagerValid   = false;
    m_Debug_SetComplete    = false;
    m_Debug_ModifierFound  = false;
    m_Debug_WriteApplied   = false;
    m_Debug_PiecesMatched  = 0;
    m_Debug_WriteCount     = 0;
    m_Debug_LastError      = nullptr;
    for (int i = 0; i < 6; i++) { m_Debug_SlotLineIds[i] = 0; m_Debug_SlotMatched[i] = false; }
}

void GearSetPerkPlugin::OnUpdate()
{
    // ---- refresh debug state (always runs, even when disabled) ----
    m_Debug_ManagerValid  = false;
    m_Debug_SetComplete   = false;
    m_Debug_ModifierFound = false;
    m_Debug_WriteApplied  = false;
    m_Debug_PiecesMatched = 0;
    m_Debug_LastError     = nullptr;
    for (int i = 0; i < 6; i++) { m_Debug_SlotLineIds[i] = 0; m_Debug_SlotMatched[i] = false; }

    const int setCount = SetCatalogCount();
    if (setCount <= 0) { m_Debug_LastError = "set catalog empty"; return; }
    const GearSetDef& set = kGearSets[ClampIndex(m_SetIndex, setCount)];

    __try
    {
        AvatarGearManager* agm = AvatarGearManager::GetSingleton();
        if (!agm) { m_Debug_LastError = "AvatarGearManager singleton null"; return; }
        m_Debug_ManagerValid = true;

        // Six embedded AvatarGear objects = currently equipped gear.
        AvatarGear* slots[6] = {
            &agm->gear_38, &agm->gear_D0, &agm->gear_168,
            &agm->gear_200, &agm->gear_298, &agm->gear_330,
        };

        int matchedSlots[6] = { -1, -1, -1, -1, -1, -1 };
        int piecesMatched = 0;

        for (int i = 0; i < 6; i++)
        {
            AvatarGear* g = slots[i];
            if (!g) { continue; }
            m_Debug_SlotLineIds[i] = g->uiString_gearName.stringID;

            for (int slotIdx = 0; slotIdx < 5; slotIdx++)
            {
                for (int r = 0; r < 4; r++)
                {
                    const uint32_t id = set.pieceLineIds[slotIdx][r];
                    if (id == 0) { break; }
                    if (id == g->uiString_gearName.stringID)
                    {
                        m_Debug_SlotMatched[i] = true;
                        matchedSlots[i] = slotIdx;
                        piecesMatched++;
                    }
                }
            }
        }

        m_Debug_PiecesMatched = piecesMatched;
        m_Debug_SetComplete   = (piecesMatched >= m_RequiredPieces);

        if (!m_Enabled || !m_Debug_SetComplete) { return; }

        // ---- apply the perk ----
        if (m_PerkAmount < 0) { m_Debug_LastError = "perk amount must be >= 0"; return; }

        const int perkCount = PerkCatalogCount();
        if (perkCount <= 0) { m_Debug_LastError = "perk catalog empty"; return; }
        const PerkDef& perk = kPerks[ClampIndex(m_PerkTypeIndex, perkCount)];

        // Target piece: the first matched embedded gear slot.
        AvatarGear* target = nullptr;
        int targetSlot = -1;
        for (int i = 0; i < 6; i++)
        {
            if (matchedSlots[i] >= 0) { target = slots[i]; targetSlot = i; break; }
        }
        if (!target) { m_Debug_LastError = "no matched piece to write to"; return; }

        // Find the perk modifier inside the piece's Modifiers array.
        for (AvatarGearModifier* rawMod : target->Modifiers)
        {
            GearModifierLayout* mod = (GearModifierLayout*)rawMod;
            if (!mod) { continue; }
            if (mod->displayNameId != perk.modifierDisplayLineId) { continue; }

            m_Debug_ModifierFound = true;
            if (mod->value != (uint32_t)m_PerkAmount)
            {
                mod->value = (uint32_t)m_PerkAmount;
                m_Debug_WriteApplied = true;
                m_Debug_WriteCount++;
                LOG_DEBUG(DefaultLogger, "[GearSetPerk] Applied perk \"%s\" (+%d) to piece (lineId %u, slot %d).\n",
                    perk.name, m_PerkAmount, m_Debug_SlotLineIds[targetSlot], targetSlot);
            }
            break;
        }

        if (!m_Debug_ModifierFound)
        {
            m_Debug_LastError = "perk modifier not found on equipped piece (layout mismatch?)";
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // stale/mutated pointer chain - never crash the game
        m_Debug_ManagerValid  = false;
        m_Debug_SetComplete   = false;
        m_Debug_LastError     = "SEH: stale pointer chain";
    }
}

void GearSetPerkPlugin::SaveConfigToFile()
{
    g_Config.gearSetPerk.enabled        = m_Enabled;
    g_Config.gearSetPerk.setIndex       = m_SetIndex;
    g_Config.gearSetPerk.requiredPieces = m_RequiredPieces;
    g_Config.gearSetPerk.perkTypeIndex  = m_PerkTypeIndex;
    g_Config.gearSetPerk.perkAmount     = m_PerkAmount;
    MainConfig::WriteToFile();
}

void GearSetPerkPlugin::OnImGuiRender()
{
    if (ImGui::Checkbox("Enable gear-set perk", &m_Enabled)) { SaveConfigToFile(); }

    ImGui::Separator();

    const int setCount  = SetCatalogCount();
    const int perkCount = PerkCatalogCount();
    const int setIdx    = ClampIndex(m_SetIndex, setCount);
    const int perkIdx   = ClampIndex(m_PerkTypeIndex, perkCount);

    if (ImGui::BeginCombo("Armor set", kGearSets[setIdx].name))
    {
        for (int i = 0; i < setCount; i++)
        {
            if (ImGui::Selectable(kGearSets[i].name, m_SetIndex == i)) { m_SetIndex = i; SaveConfigToFile(); }
        }
        ImGui::EndCombo();
    }

    if (ImGui::SliderInt("Pieces required", &m_RequiredPieces, 1, 5, "%d / 5"))
    {
        SaveConfigToFile();
    }

    if (ImGui::BeginCombo("Perk", kPerks[perkIdx].name))
    {
        for (int i = 0; i < perkCount; i++)
        {
            if (ImGui::Selectable(kPerks[i].name, m_PerkTypeIndex == i)) { m_PerkTypeIndex = i; SaveConfigToFile(); }
        }
        ImGui::EndCombo();
    }

    if (ImGui::InputInt("Perk amount", &m_PerkAmount)) { SaveConfigToFile(); }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Gear-set debug readout"))
    {
        ImGui::Text("Manager valid:      %s", m_Debug_ManagerValid ? "yes" : "no");
        ImGui::Text("Pieces matched:     %d / %d", m_Debug_PiecesMatched, m_RequiredPieces);
        ImGui::Text("Set complete:       %s", m_Debug_SetComplete ? "yes" : "no");
        ImGui::Text("Perk modifier found:%s", m_Debug_ModifierFound ? "yes" : "no");
        ImGui::Text("Write applied:      %s (%d total)", m_Debug_WriteApplied ? "yes" : "no", m_Debug_WriteCount);
        if (m_Debug_LastError) { ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Last error: %s", m_Debug_LastError); }
        for (int i = 0; i < 6; i++)
        {
            ImGui::Text("Slot %d: lineID=%6u  %s", i, m_Debug_SlotLineIds[i],
                m_Debug_SlotMatched[i] ? "[SET]" : "");
        }
    }
}
