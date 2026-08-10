#include "pch.h"
#include "DodgeAnywherePlugin.h"
#include <Windows.h>
#include "MyLog.h"

constexpr uintptr_t ADDR_PLAYER_PROGRESSION_MANAGER = 0x1451B3C58;
constexpr uintptr_t OFFSET_MANAGER_TO_ABILITYSET    = 0x4E8;

constexpr uintptr_t BYTE_OFFSET_COUNTERDODGE      = 0x2A;
constexpr uintptr_t BYTE_OFFSET_CANDODGERANGEATKS = 0x2A;
constexpr uintptr_t BYTE_OFFSET_COMBATSTEPS       = 0x2B;
constexpr uint8_t   BIT_COUNTERDODGE              = 1 << 1;
constexpr uint8_t   BIT_CANDODGERANGEATKS         = 1 << 6;
constexpr uint8_t   BIT_COMBATSTEPS               = 1 << 2;

static bool g_gameReady = false;

static bool IsPlausiblePointer(uintptr_t p)
{
    return p > 0x10000 && p < 0x0007FFFFFFFFFFFFull;
}

static uintptr_t GetAbilitySet()
{
    uintptr_t mgr = *(uintptr_t*)ADDR_PLAYER_PROGRESSION_MANAGER;
    if (!IsPlausiblePointer(mgr)) return 0;
    uintptr_t abilitySet = *(uintptr_t*)(mgr + OFFSET_MANAGER_TO_ABILITYSET);
    if (!IsPlausiblePointer(abilitySet)) return 0;
    return abilitySet;
}

void DodgeAnywherePlugin::OnUpdate()
{
    if (!g_gameReady)
    {
        if (!GetAbilitySet()) return;
        g_gameReady = true;
        LOG_DEBUG(DefaultLogger, "[DodgeAnywhere] Game world ready.");
    }

    if (m_RestoreNextFrame)
    {
        m_RestoreNextFrame = false;
        uintptr_t abilitySet = GetAbilitySet();
        if (abilitySet)
        {
            uint8_t valA = *(uint8_t*)(abilitySet + BYTE_OFFSET_COUNTERDODGE);
            if (m_SavedCounterDodge) valA |=  BIT_COUNTERDODGE;
            else                     valA &= ~BIT_COUNTERDODGE;
            *(uint8_t*)(abilitySet + BYTE_OFFSET_COUNTERDODGE) = valA;

            if (m_SavedCanDodgeRange) valA |=  BIT_CANDODGERANGEATKS;
            else                      valA &= ~BIT_CANDODGERANGEATKS;
            *(uint8_t*)(abilitySet + BYTE_OFFSET_CANDODGERANGEATKS) = valA;

            uint8_t valB = *(uint8_t*)(abilitySet + BYTE_OFFSET_COMBATSTEPS);
            if (m_SavedCombatSteps) valB |=  BIT_COMBATSTEPS;
            else                    valB &= ~BIT_COMBATSTEPS;
            *(uint8_t*)(abilitySet + BYTE_OFFSET_COMBATSTEPS) = valB;
        }
    }

    if (!m_Enabled) return;

    if (m_WaitingForKey)
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
        {
            if (GetAsyncKeyState(vk) & 0x8000)
            {
                m_Key = vk;
                m_WaitingForKey = false;
                break;
            }
        }
        return;
    }

    static bool prevDown = false;
    bool down = (GetAsyncKeyState(m_Key) & 0x8000) != 0;
    if (down && !prevDown)
    {
        uintptr_t abilitySet = GetAbilitySet();
        if (abilitySet)
        {
            uint8_t valA = *(uint8_t*)(abilitySet + BYTE_OFFSET_COUNTERDODGE);
            m_SavedCounterDodge  = (valA & BIT_COUNTERDODGE)     != 0;
            m_SavedCanDodgeRange = (valA & BIT_CANDODGERANGEATKS) != 0;

            uint8_t valB = *(uint8_t*)(abilitySet + BYTE_OFFSET_COMBATSTEPS);
            m_SavedCombatSteps = (valB & BIT_COMBATSTEPS) != 0;

            valA |= BIT_COUNTERDODGE | BIT_CANDODGERANGEATKS;
            *(uint8_t*)(abilitySet + BYTE_OFFSET_COUNTERDODGE) = valA;

            valB |= BIT_COMBATSTEPS;
            *(uint8_t*)(abilitySet + BYTE_OFFSET_COMBATSTEPS) = valB;

            m_RestoreNextFrame = true;
            LOG_DEBUG(DefaultLogger, "[DodgeAnywhere] Dodge enabled for this frame.");
        }
    }
    prevDown = down;
}

void DodgeAnywherePlugin::OnImGuiRender()
{
    if (!ImGui::Begin("Dodge Anywhere"))
    {
        ImGui::End();
        return;
    }
    ImGui::TextColored(m_Enabled ? ImVec4(0.2f,1,0.2f,1) : ImVec4(1,0.4f,0.4f,1),
                       m_Enabled ? "Enabled" : "Disabled");
    ImGui::Checkbox("Enable dodge anywhere", &m_Enabled);
    if (m_WaitingForKey)
        ImGui::TextColored({1,1,0,1}, "Press any key to bind...");
    else
    {
        ImGui::Text("Dodge key: VK 0x%X", m_Key);
        ImGui::SameLine();
        if (ImGui::Button("Rebind##dodge"))
            m_WaitingForKey = true;
    }
    ImGui::End();
}