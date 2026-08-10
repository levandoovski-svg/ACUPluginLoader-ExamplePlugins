#include "pch.h"
#include "StunBladePlugin.h"

#include <cstdio>

#include "ACU/HumanStatesHolder.h"
#include "ACU/Enum_EquipmentType.h"

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
// ===========================================================================

static constexpr EquipmentType kBladeType    = EquipmentType::PhantomBlade; // 0x16
static constexpr EquipmentType kStunBombType = EquipmentType::StunBomb;     // 0x14

static volatile bool g_Enabled              = true;
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
    if (!g_Enabled)
    {
        return;
    }

    // Brute force: every single frame, while the player's ballistic aim is a
    // PhantomBlade, make it a StunBomb. If no player / no holder yet, skip.
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
}