#include "pch.h"
#include "FreeCameraRotationPlugin.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/InputContainer.h"
#include "ACU/World.h"
#include "Common_Plugins/ACU_InputUtils.h"

static constexpr uint64_t ADDR_Assassination_PP         = 0x141A4A4B0;
static constexpr uint64_t ADDR_Assassination_P          = 0x141A462F0;
static constexpr uint64_t ADDR_Assassination_FirstHalf  = 0x141A42F60;
static constexpr uint64_t ADDR_Assassination_SecondHalf = 0x141A40BE0;

static constexpr float PI = 3.14159265358979323846f;

FreeCameraRotationPlugin g_FreeCameraRotation;

static float BringToIntervalWithWraparound(float current, float min_, float max_)
{
    const float interval = max_ - min_;
    while (current >= max_) current -= interval;
    while (current < min_) current += interval;
    return current;
}

bool FreeCameraRotationPlugin::IsInAssassinationState()
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

DEFINE_GAME_FUNCTION(World__SetUnpausedGameTimescale_onSlowMotion, 0x141D5E210,
    void, __fastcall, (World* world, float newTimescale));

struct CameraRotationHook : AutoAssemblerCodeHolder_Base
{
    CameraRotationHook()
    {
        uintptr_t whenSettingFOVforFrame = 0x141F3FE3B;
        PresetScript_CCodeInTheMiddle(whenSettingFOVforFrame, 6,
            [](AllRegisters* params)
            {
                auto* cam = (ACUPlayerCameraComponent*)params->r14_;
                if (!cam) return;

                if (!g_FreeCameraRotation.m_Enabled) return;

                bool inAssassination = g_FreeCameraRotation.IsInAssassinationState();

                if (inAssassination)
                {
                    if (!g_FreeCameraRotation.m_WasInAssassination)
                    {
                        if (g_FreeCameraRotation.m_SlowMotion && !g_FreeCameraRotation.m_TimescaleApplied)
                        {
                            World* w = World::GetSingleton();
                            if (w)
                            {
                                World__SetUnpausedGameTimescale_onSlowMotion(w, g_FreeCameraRotation.m_SlowMotionTimescale);
                                g_FreeCameraRotation.m_TimescaleApplied = true;
                            }
                        }
                    }

                    InputContainerBig* inp = ACU::Input::Get_InputContainerBig();
                    if (inp)
                    {
                        float dx = (float)inp->mouseState.mouseDeltaIntForCamera_X;
                        float dy = (float)inp->mouseState.mouseDeltaIntForCamera_Y;

                        if (dx != 0.0f || dy != 0.0f)
                        {
                            float angleZ = cam->spinaroundAngleZtarget;
                            float angleV = cam->spinaroundAngleUpDownTarget;

                            float xMult = g_FreeCameraRotation.m_InvertX ? 1.0f : -1.0f;
                            float yMult = g_FreeCameraRotation.m_InvertY ? -1.0f : 1.0f;

                            angleZ += dx * 0.003f * xMult;
                            angleV += dy * 0.003f * yMult;

                            angleZ = BringToIntervalWithWraparound(angleZ, -PI, PI);

                            constexpr float verticalLimit = 1.48f;
                            if (angleV > verticalLimit) angleV = verticalLimit;
                            if (angleV < -verticalLimit) angleV = -verticalLimit;

                            cam->spinaroundAngleZtarget = angleZ;
                            cam->spinaroundAngleUpDownTarget = angleV;
                        }
                    }
                }
                else
                {
                    if (g_FreeCameraRotation.m_TimescaleApplied)
                    {
                        World* w = World::GetSingleton();
                        if (w)
                            World__SetUnpausedGameTimescale_onSlowMotion(w, 1.0f);
                        g_FreeCameraRotation.m_TimescaleApplied = false;
                    }
                }

                g_FreeCameraRotation.m_WasInAssassination = inAssassination;
            },
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    }
};

static std::unique_ptr<AutoAssembleWrapper<CameraRotationHook>> g_CameraRotationHook;

void FreeCameraRotationPlugin::Initialize()
{
    g_CameraRotationHook = std::make_unique<AutoAssembleWrapper<CameraRotationHook>>();
    g_CameraRotationHook->Activate();
}

void FreeCameraRotationPlugin::OnUpdate()
{
}

void FreeCameraRotationPlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Free Camera Rotation"))
        return;

    ImGui::Checkbox("Enable Free Camera During Assassinations", &m_Enabled);
    if (!m_Enabled) return;

    ImGui::Indent();

    ImGui::Checkbox("Invert X", &m_InvertX);
    ImGui::Checkbox("Invert Y", &m_InvertY);

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
