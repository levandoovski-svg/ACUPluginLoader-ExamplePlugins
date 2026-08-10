#include "pch.h"
#include "SocialStealthPlugin.h"
#include "ACU/VanishingManager.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/BhvAssassin.h"

static bool IsBlending()
{
    VanishingManager* vm = VanishingManager::GetSingleton();
    return vm && vm->_8permanentBlend2crowdBlend0x40moneyPouch == 2;
}

static void SetInvisible(bool invisible)
{
    BhvAssassin* bhv = ACU::GetPlayerBhvAssassin();
    if (bhv)
        ((AIComponent*)bhv)->bInvisible = invisible ? 1 : 0;
}

void SocialStealthPlugin::LoadSettings() {}

void SocialStealthPlugin::OnUpdate()
{
    if (!m_Enabled) return;

    bool blending = IsBlending();

    // Rising edge: started blending → go invisible
    if (blending && !m_WasBlending)
        SetInvisible(true);

    // Falling edge: stopped blending → visible again
    if (!blending && m_WasBlending)
        SetInvisible(false);

    m_WasBlending = blending;
}

void SocialStealthPlugin::OnImGuiRender()
{
    ImGui::Checkbox("Social Stealth (silent blend)", &m_Enabled);
}
