#include "pch.h"
#include "ForceSheathePlugin.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/Entity.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU_DefineNativeFunction.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>

// Uplay 1.5.0 — verify this address is the Uplay equivalent
// before shipping; if it crashes revert DoSheathe() to a no-op
DEFINE_GAME_FUNCTION(ReattachWeaponToSheathOrHolster, 0x141B05570,
    void, __fastcall, (__int64 a1, int p_1melee2ranged));

// EquipWeapon leaf state addresses (same in both versions per agent research)
static constexpr uint64_t ADDR_EQUIPWEAPON_EQUIPPING         = 0x141B03750;
static constexpr uint64_t ADDR_EQUIPWEAPON_PENDINGTRANSITION = 0x141B04C30;

static bool IsStateActive(HumanStatesHolder& hs, uint64_t addr)
{
    for (auto& r : hs.primaryCallbackReceivers)
        if (r.pNode && (uint64_t)r.pNode->Enter == addr)
            return true;
    return false;
}

void ForceSheathePlugin::DoSheathe()
{
    Entity* player = ACU::GetPlayer();
    if (!player) return;
    ReattachWeaponToSheathOrHolster((__int64)player, 1);
    ReattachWeaponToSheathOrHolster((__int64)player, 2);
}

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\ForceSheathe.ini";
    return "ForceSheathe.ini";
}

void ForceSheathePlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("SheatheKey=", 0) == 0)
            try { m_SheatheKey = std::stoi(line.substr(11), nullptr, 16); } catch (...) {}
        if (line.rfind("PreventDrawKey=", 0) == 0)
            try { m_PreventDrawKey = std::stoi(line.substr(15), nullptr, 16); } catch (...) {}
        if (line.rfind("PreventDraw=", 0) == 0)
            try { m_PreventDraw = std::stoi(line.substr(12)) != 0; } catch (...) {}
    }
}

void ForceSheathePlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (f)
            f << "SheatheKey=" << std::hex << m_SheatheKey << "\n"
              << "PreventDrawKey=" << m_PreventDrawKey << "\n"
              << std::dec << "PreventDraw=" << (m_PreventDraw ? 1 : 0) << "\n";
    } catch (...) {}
}

void ForceSheathePlugin::OnUpdate()
{
    // ── Rebind sheathe key ───────────────────────────────
    if (m_WaitingForSheathe)
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000)
            {
                m_SheatheKey = vk; m_WaitingForSheathe = false;
                SaveSettings(); return;
            }
        return;
    }

    // ── Rebind prevent-draw key ──────────────────────────
    if (m_WaitingForDrawKey)
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000)
            {
                m_PreventDrawKey = vk; m_WaitingForDrawKey = false;
                SaveSettings(); return;
            }
        return;
    }

    // ── Force sheathe — rising edge ──────────────────────
    bool sheatheDown = (GetAsyncKeyState(m_SheatheKey) & 0x8000) != 0;
    if (sheatheDown && !m_PrevSheatheKey)
        DoSheathe();
    m_PrevSheatheKey = sheatheDown;

    // ── Prevent draw toggle — rising edge ────────────────
    bool drawKeyDown = (GetAsyncKeyState(m_PreventDrawKey) & 0x8000) != 0;
    if (drawKeyDown && !m_PrevDrawKey)
    {
        m_PreventDraw = !m_PreventDraw;
        SaveSettings();
    }
    m_PrevDrawKey = drawKeyDown;

    // ── Prevent draw — cancel equip every frame ──────────
    if (m_PreventDraw)
    {
        HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
        if (hs && (IsStateActive(*hs, ADDR_EQUIPWEAPON_EQUIPPING) ||
                   IsStateActive(*hs, ADDR_EQUIPWEAPON_PENDINGTRANSITION)))
            DoSheathe();
    }
}

void ForceSheathePlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Force Sheathe"))
        return;

    // Force sheathe key
    ImGui::Text("Force Sheathe: 0x%02X", m_SheatheKey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForSheathe ? "Cancel##s" : "Rebind##s"))
        m_WaitingForSheathe = !m_WaitingForSheathe;
    if (m_WaitingForSheathe)
        ImGui::TextColored({1,1,0,1}, "Press any key...");

    ImGui::Separator();

    // Prevent draw toggle
    ImVec4 col = m_PreventDraw ? ImVec4(1,0.3f,0.3f,1) : ImVec4(0.5f,0.5f,0.5f,1);
    ImGui::TextColored(col, m_PreventDraw ? "Draw: LOCKED" : "Draw: unlocked");
    ImGui::Text("Toggle key: 0x%02X", m_PreventDrawKey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForDrawKey ? "Cancel##d" : "Rebind##d"))
        m_WaitingForDrawKey = !m_WaitingForDrawKey;
    if (m_WaitingForDrawKey)
        ImGui::TextColored({1,1,0,1}, "Press any key...");
}
