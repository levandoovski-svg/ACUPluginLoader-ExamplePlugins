#include "pch.h"
#include "AllowSmokeAssassinatePlugin.h"
#include "ACU/HumanStatesHolder.h"

bool g_SmokeBombAssaultEnabled = false;

static float GetTimeSeconds()
{
    return static_cast<float>(GetTickCount64()) / 1000.0f;
}

static bool IsDroppingBomb()
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) return false;
    for (auto& r : hs->primaryCallbackReceivers)
        if (r.pNode && (uint64_t)r.pNode->Enter == 0x141AA7D90)
            return true;
    return false;
}

static float g_SmokeBombTime = 0.0f;
static bool  g_PrevBombState = false;

void AllowSmokeAssassinatePlugin::LoadSettings() {}

void AllowSmokeAssassinatePlugin::OnUpdate()
{
    if (!m_Enabled) return;

    bool bombNow    = IsDroppingBomb();
    bool risingEdge = bombNow && !g_PrevBombState;
    g_PrevBombState = bombNow;

    if (risingEdge)
    {
        g_SmokeBombTime = GetTimeSeconds();
        g_SmokeBombAssaultEnabled = true;
    }

    if (g_SmokeBombAssaultEnabled &&
        (GetTimeSeconds() - g_SmokeBombTime) > 7.0f)
    {
        g_SmokeBombAssaultEnabled = false;
    }
}

void AllowSmokeAssassinatePlugin::OnImGuiRender()
{
    ImGui::Checkbox("Smoke Bomb Assault", &m_Enabled);
}
