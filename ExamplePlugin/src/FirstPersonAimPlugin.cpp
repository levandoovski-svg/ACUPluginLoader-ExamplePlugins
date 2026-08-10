#include "pch.h"
#include "FirstPersonAimPlugin.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/Entity.h"

static constexpr float PI = 3.14159265358979323846f;

static constexpr uint64_t ADDR_AIMGUN_AIMING    = 0x141AA4DD0;
static constexpr uint64_t ADDR_AIMGUN_AIMINGP   = 0x141AA6350;
static constexpr uint64_t ADDR_AIMGUN_PARENT    = 0x141AA68F0;

bool FirstPersonAimPlugin::IsAimingGun()
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) return false;

    __try {
        for (auto& r : hs->primaryCallbackReceivers)
        {
            if (!r.pNode) continue;
            uint64_t addr = (uint64_t)r.pNode->Enter;
            if (addr == ADDR_AIMGUN_AIMING || addr == ADDR_AIMGUN_AIMINGP || addr == ADDR_AIMGUN_PARENT)
            {
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return false;
}

void FirstPersonAimPlugin::LoadSettings()
{
}

void FirstPersonAimPlugin::OnUpdate()
{
    if (!m_Enabled) return;

    __try {
        bool isAiming = IsAimingGun();

        ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
        Entity* player = ACU::GetPlayer();

        if (!cam || !player) return;

        if (isAiming)
        {
            cam->fov_mb_pi_4 = m_FovZoom;
            cam->fovPrecalc = m_FovZoom * (PI / 4.0f);

            if (m_HidePlayer)
            {
                m_SavedPlayerFlags88 = *(uint64*)&player->flags88;
                player->flags88.IsHidden = 1;
            }
        }
        else if (m_WasAiming)
        {
            if (m_HidePlayer)
            {
                *(uint64*)&player->flags88 = m_SavedPlayerFlags88;
            }
        }

        m_WasAiming = isAiming;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void FirstPersonAimPlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("First Person Aim"))
        return;

    ImGui::Checkbox("Enable First Person Aim", &m_Enabled);
    if (!m_Enabled) return;

    ImGui::Indent();

    ImGui::SliderFloat("FOV Zoom", &m_FovZoom, 0.1f, 1.0f, "%.2f");
    ImGui::Checkbox("Hide Player Body", &m_HidePlayer);

    ImGui::Unindent();
}
