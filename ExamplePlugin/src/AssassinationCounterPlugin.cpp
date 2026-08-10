#include "pch.h"
#include "AssassinationCounterPlugin.h"

#include "ACU/ACUGetSingletons.h"
#include "ACU/CSrvPlayerHealth.h"
#include "ACU/HumanStatesHolder.h"

namespace
{
constexpr uint64_t kAssassinationStates[] = {
    0x141A4A4B0, // Assassination_PP
    0x141A462F0, // Assassination_P
    0x141A40BE0, // Assassination_SecondHalf_mb
};

bool IsTrackedEnter(uint64_t enter)
{
    for (const uint64_t trackedEnter : kAssassinationStates)
    {
        if (enter == trackedEnter)
            return true;
    }
    return false;
}

bool IsAssassinationReceiver(FunctorBase* node)
{
    return node && IsTrackedEnter(reinterpret_cast<uintptr_t>(node->Enter));
}
} // namespace

bool AssassinationCounterPlugin::IsAssassinationStateActive() const
{
    __try
    {
        HumanStatesHolder* humanStates = HumanStatesHolder::GetForPlayer();
        if (!humanStates)
            return false;

        // Match the proven ParryAssassinPlugin pattern: inspect active leaf
        // receivers directly instead of walking the opaque root object.
        for (const auto& receiver : humanStates->primaryCallbackReceivers)
        {
            if (IsAssassinationReceiver(receiver.pNode))
                return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return false;
}

void AssassinationCounterPlugin::OnUpdate()
{
    if (!m_Enabled)
        return;

    const bool stateActive = IsAssassinationStateActive();
    const bool stateEntered = stateActive && !m_PreviousStateActive;
    m_PreviousStateActive = stateActive;

    if (!stateEntered)
        return;

    ++m_Counter;
    if (m_Counter > 1)
        TriggerDesynchronization();
}

void AssassinationCounterPlugin::OnImGuiRender()
{
    ImGui::Checkbox("Assassination Counter Enabled", &m_Enabled);
    if (!m_Enabled)
        return;

    ImGui::Separator();
    ImGui::Text("Counter: %u", m_Counter);

    if (ImGui::Button("Reset Counter"))
        m_Counter = 0;

    ImGui::Separator();
    ImGui::Text("Tracked Assassination States:");
    ImGui::BulletText("0x141A4A4B0 - Assassination_PP");
    ImGui::BulletText("0x141A462F0 - Assassination_P");
    ImGui::BulletText("0x141A40BE0 - Assassination_SecondHalf_mb");
}

void AssassinationCounterPlugin::TriggerDesynchronization()
{
    __try
    {
        CSrvPlayerHealth* health = ACU::GetPlayerHealth();
        if (health)
            health->isDesynchronizationNow = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // The player health object can disappear during scene/player changes.
    }
}