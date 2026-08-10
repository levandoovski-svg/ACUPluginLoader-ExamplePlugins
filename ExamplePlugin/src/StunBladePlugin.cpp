#include "pch.h"
#include "StunBladePlugin.h"

#include <cstdio>

#include "ACU/HumanStatesHolder.h"
#include "ACU/Enum_EquipmentType.h"
#include "ACU/ACUGetSingletons.h"   // ACU::GetPlayer() for the owner==player check

// ===========================================================================
// StunBladePlugin - BRUTE-FORCE VERSION
// ===========================================================================
// Every frame (OnUpdate runs from both frame callbacks, even when the ImGui
// menu is closed), we grab the player's HumanStatesHolder and, if the
// current ballistic equipment type is the PhantomBlade, rewrite it to
// StunBomb. Because this runs every frame BEFORE the game resolves what to
// spawn, whichever dispatch path the engine uses (throw bomb, quickshot,
// anything) will read StunBomb and spawn a genuine stun bomb.
//
// This version has NO hooks at all - no asm, no stolen bytes, no function
// entries, nothing that can crash. Pure per-frame field write, as brute
// force as it gets. "Works first" > cleverness (the throw-dispatcher entry
// hook 0x141988480 crashed with C0000005 on 2026-08-08; we delete that
// approach entirely).
//
// All game-memory reads are SEH-guarded: a stale/mutated pointer skips the
// frame instead of taking the game down (DesyncKeybindPlugin pattern).
// ===========================================================================

static constexpr EquipmentType kBladeType    = EquipmentType::PhantomBlade; // 0x16
static constexpr EquipmentType kStunBombType = EquipmentType::StunBomb;     // 0x14

static volatile bool g_Enabled               = true;
static volatile unsigned long long g_Rewrites = 0; // times we swapped the field

void StunBladePlugin::OnBeforeActivate()
{
}

void StunBladePlugin::OnBeforeDeactivate()
{
    g_Enabled = false;
}

void StunBladePlugin::OnUpdate()
{
    g_Enabled = m_Enabled;

    // ---- debug readout: refresh every frame, even while disabled ----
    __try
    {
        HumanStatesHolder* humanStates = HumanStatesHolder::GetForPlayer();
        m_Debug_HumanStatesValid     = (humanStates != nullptr);
        m_Debug_OwnerEntityValid     = false;
        m_Debug_OwnerIsPlayer        = false;
        m_Debug_CurrentEquipmentType = 0;

        if (humanStates)
        {
            m_Debug_CurrentEquipmentType = (uint32_t)humanStates->ballisticAimingCurrentEquipmentType;
            Entity* owner = humanStates->ownerEntity;
            m_Debug_OwnerEntityValid = (owner != nullptr);
            m_Debug_OwnerIsPlayer    = (owner != nullptr && owner == ACU::GetPlayer());
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // stale chain - report NULLs this frame, never crash the game
        m_Debug_HumanStatesValid     = false;
        m_Debug_OwnerEntityValid     = false;
        m_Debug_OwnerIsPlayer        = false;
        m_Debug_CurrentEquipmentType = 0;
    }

    if (!g_Enabled)
    {
        return;
    }

    // ---- brute-force rewrite ----
    __try
    {
        HumanStatesHolder* humanStates = HumanStatesHolder::GetForPlayer();
        if (!humanStates)
        {
            return;
        }

        if (humanStates->ballisticAimingCurrentEquipmentType == kBladeType)
        {
            humanStates->ballisticAimingCurrentEquipmentType = kStunBombType;
            ++g_Rewrites;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // transient bad pointer - skip this frame
    }
}

void StunBladePlugin::OnImGuiRender()
{
    ImGui::Text("Stun Blade (brute force: PhantomBlade -> StunBomb every frame)");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0, 1, 0, 1), g_Enabled ? "ON" : "OFF");

    ImGui::SeparatorText("Behavior");
    ImGui::Checkbox("Enabled", &m_Enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While ON, every frame the player's ballistic "
                          "equipment is forced to StunBomb whenever it is "
                          "the phantom blade.");

    ImGui::SeparatorText("Diagnostics");
    ImGui::Text("Field rewrites: %llu", g_Rewrites);
    if (ImGui::Button("Reset counters"))
    {
        g_Rewrites = 0;
    }

    ImGui::SeparatorText("Debug readout (addresses / pointers)");
    ImGui::Text("HumanStatesHolder::GetForPlayer(): %s",
                m_Debug_HumanStatesValid ? "OK" : "NULL");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("NULL here means the singleton/player chain did not "
                          "resolve. Expected NULL in the main menu - read in "
                          "gameplay.");
    ImGui::Text("holder->ownerEntity: %s", m_Debug_OwnerEntityValid ? "OK" : "NULL");
    ImGui::Text("owner == ACU::GetPlayer(): %s",
                m_Debug_OwnerIsPlayer ? "yes" : "no");
    ImGui::Text("ballisticAimingCurrentEquipmentType: 0x%X (%s)",
                m_Debug_CurrentEquipmentType,
                m_Debug_CurrentEquipmentType == (uint32_t)kBladeType ? "PhantomBlade - will swap"
                : m_Debug_CurrentEquipmentType == (uint32_t)kStunBombType ? "StunBomb - already swapped"
                : "other");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Aim the throwblade and this should read 0x16 "
                          "(PhantomBlade). If it reads 0x16 but rewrites stay "
                          "0, the field is being read elsewhere (wrong build).");
}
