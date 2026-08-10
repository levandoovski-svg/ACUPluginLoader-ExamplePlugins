#include "pch.h"
#include "DisableCameraLockPlugin.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/World.h"
#include "ACU/Entity.h"

// Assassination state Enter function addresses
static constexpr uint64_t ADDR_Assassination_PP         = 0x141A4A4B0;
static constexpr uint64_t ADDR_Assassination_P           = 0x141A462F0;
static constexpr uint64_t ADDR_Assassination_FirstHalf   = 0x141A42F60;
static constexpr uint64_t ADDR_Assassination_SecondHalf  = 0x141A40BE0;

// Slow-motion game function (Steam)
DEFINE_GAME_FUNCTION(World__SetUnpausedGameTimescale_onSlowMotion, 0x141D5E210,
    void, __fastcall, (World* world, float newTimescale));

bool DisableCameraLockPlugin::IsInAssassinationState()
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) return false;

    bool found = false;
    __try {
        for (auto& r : hs->primaryCallbackReceivers)
        {
            if (!r.pNode) continue;
            uint64_t addr = (uint64_t)r.pNode->Enter;
            if (addr == ADDR_Assassination_PP || addr == ADDR_Assassination_P ||
                addr == ADDR_Assassination_FirstHalf || addr == ADDR_Assassination_SecondHalf)
            {
                found = true;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return found;
}

void DisableCameraLockPlugin::LoadSettings()
{
}

void DisableCameraLockPlugin::OnUpdate()
{
    if (!m_Enabled) return;

    __try {
        bool inState = IsInAssassinationState();

        if (inState)
        {
            // ── Camera orbit override ──
            POINT p;
            GetCursorPos(&p);
            float dx = (float)(p.x - m_PrevMouseX) * m_Sensitivity;
            float dy = (float)(p.y - m_PrevMouseY) * m_Sensitivity;
            m_PrevMouseX = p.x;
            m_PrevMouseY = p.y;

            m_CameraAngleH += m_InvertX ? -dx : dx;
            m_CameraAngleV += m_InvertY ? -dy : dy;

            if (m_CameraAngleV > 1.5f) m_CameraAngleV = 1.5f;
            if (m_CameraAngleV < -1.5f) m_CameraAngleV = -1.5f;

            Entity* player = ACU::GetPlayer();
            ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
            if (player && cam)
            {
                Vector3f playerPos = player->GetPosition();
                float cv = cosf(m_CameraAngleV);
                Vector3f offset(
                    m_CameraDistance * cv * sinf(m_CameraAngleH),
                    m_CameraDistance * sinf(m_CameraAngleV),
                    m_CameraDistance * cv * cosf(m_CameraAngleH)
                );
                cam->positionLookFrom = Vector4f(playerPos + offset, 1.0f);
                cam->locationLookat_A90 = Vector4f(playerPos + Vector3f(0, 1.5f, 0), 1.0f);
            }

            // ── Slow motion ──
            if (m_SlowMotion && !m_TimescaleApplied)
            {
                World* w = World::GetSingleton();
                if (w) {
                    World__SetUnpausedGameTimescale_onSlowMotion(w, m_SlowMotionTimescale);
                    m_TimescaleApplied = true;
                }
            }
        }
        else
        {
            // Auto-calibrate sensitivity from normal gameplay
            POINT p;
            GetCursorPos(&p);
            float pixelDx = (float)(p.x - m_PrevMouseX);
            float pixelDy = (float)(p.y - m_PrevMouseY);
            m_PrevMouseX = p.x;
            m_PrevMouseY = p.y;

            Entity* player = ACU::GetPlayer();
            ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
            if (cam)
            {
                float angleZ = cam->spinaroundAngleZtarget;
                float angleV = cam->spinaroundAngleUpDownTarget;
                float angleDx = angleZ - m_LastAngleZ;
                float angleDy = angleV - m_LastAngleV;
                m_LastAngleZ = angleZ;
                m_LastAngleV = angleV;

                if (fabsf(pixelDx) > 1.0f && fabsf(angleDx) > 0.0001f)
                {
                    float observedSensitivity = angleDx / pixelDx;
                    m_Sensitivity = m_Sensitivity * 0.95f + observedSensitivity * 0.05f;
                }
                if (fabsf(pixelDy) > 1.0f && fabsf(angleDy) > 0.0001f)
                {
                    float observedSensitivity = angleDy / pixelDy;
                    float vertSensitivity = m_Sensitivity * 0.95f + observedSensitivity * 0.05f;
                    m_Sensitivity = m_Sensitivity * 0.5f + vertSensitivity * 0.5f;
                }
            }

            // Save orbit state
            if (player && cam)
            {
                Vector3f playerPos = player->GetPosition();
                Vector3f camPos(cam->positionLookFrom.x, cam->positionLookFrom.y, cam->positionLookFrom.z);
                Vector3f diff = camPos - playerPos;
                float dist = diff.length();
                if (dist > 0.5f)
                {
                    m_CameraDistance = dist;
                    m_CameraAngleH = atan2f(diff.x, diff.z);
                    m_CameraAngleV = asinf(diff.y / dist);
                }
            }

            // Restore timescale
            if (m_TimescaleApplied)
            {
                World* w = World::GetSingleton();
                if (w)
                    World__SetUnpausedGameTimescale_onSlowMotion(w, 1.0f);
                m_TimescaleApplied = false;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void DisableCameraLockPlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Camera Lock"))
        return;

    ImGui::Checkbox("Disable Camera Lock", &m_Enabled);
    if (!m_Enabled) return;

    ImGui::Indent();

    ImGui::Text("Sensitivity: auto-calibrated (%.4f)", m_Sensitivity);
    ImGui::Checkbox("Invert Y", &m_InvertY);
    ImGui::Checkbox("Invert X", &m_InvertX);

    ImGui::Separator();

    ImGui::Checkbox("Slow Motion", &m_SlowMotion);
    if (m_SlowMotion)
    {
        ImGui::Indent();
        ImGui::SliderFloat("Timescale", &m_SlowMotionTimescale, 0.01f, 1.0f, "%.2f");
        ImGui::Unindent();
    }

    ImGui::Unindent();
}
