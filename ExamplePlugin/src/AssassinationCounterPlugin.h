#pragma once

#include <cstdint>

class AssassinationCounterPlugin
{
public:
    void OnUpdate();
    void OnImGuiRender();

private:
    bool IsAssassinationStateActive() const;
    void TriggerDesynchronization();

    bool m_Enabled = true;
    bool m_PreviousStateActive = false;
    uint32_t m_Counter = 0;
};