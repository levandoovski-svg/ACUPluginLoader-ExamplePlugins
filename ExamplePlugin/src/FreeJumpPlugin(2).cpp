#include "pch.h"

#include "FreeJumpPlugin.h"
#include "MyLog.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include <fstream>
#include <sstream>
#include <Windows.h>
#include <shlobj.h>     // for SHGetFolderPathA

// ======== Settings persistence (safe location) ========

static std::string GetGameSaveFolder()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        std::string path(documents);
        path += "\\Assassin's Creed Unity\\FreeJumpPlugin.ini";
        return path;
    }
    return "FreeJumpPlugin.ini"; // fallback, unlikely to be hit
}

void FreeJumpPlugin::LoadSettings()
{
    std::ifstream file(GetGameSaveFolder());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("FreeJumpKey=", 0) == 0)
        {
            try {
                m_ToggleKey = std::stoi(line.substr(13), nullptr, 16);
            } catch (...) {
                // ignore corrupt value, keep default
            }
        }
        else if (line.rfind("CenterScreenKey=", 0) == 0)
        {
            try {
                m_CenterScreenKey = std::stoi(line.substr(16), nullptr, 16);
            } catch (...) {}
        }
        else if (line.rfind("CenterScreenThreshold=", 0) == 0)
        {
            try {
                m_CenterScreenThreshold = std::stof(line.substr(22));
            } catch (...) {}
        }
        else if (line.rfind("CenterScreenMode=", 0) == 0)
        {
            m_CenterScreenMode = (line.substr(17) == "1");
        }
    }
}

void FreeJumpPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetGameSaveFolder());
        if (file)
            file << "FreeJumpKey=" << std::hex << m_ToggleKey << std::dec << "\n"
                 << "CenterScreenKey=" << std::hex << m_CenterScreenKey << std::dec << "\n"
                 << "CenterScreenThreshold=" << m_CenterScreenThreshold << "\n"
                 << "CenterScreenMode=" << (m_CenterScreenMode ? 1 : 0) << "\n";
    } catch (...) {
        // silently fail if we can't save (e.g. disk full)
    }
}

// Remove the old GetIniPath() function – it’s no longer needed.

// ======== Camera helpers for center-screen targeting ========

static Vector4f GetCameraPosition()
{
    auto* cam = ACUPlayerCameraComponent::GetSingleton();
    if (!cam) return { 0.0f, 0.0f, 0.0f, 0.0f };
    return *(Vector4f*)((uintptr_t)cam + 0x50);
}

static Vector4f GetCameraForward3D()
{
    auto* cam = ACUPlayerCameraComponent::GetSingleton();
    if (!cam) return { 0.0f, 0.0f, 1.0f, 0.0f };
    float x = *(float*)((uintptr_t)cam + 0x60);
    float y = *(float*)((uintptr_t)cam + 0x64);
    float z = *(float*)((uintptr_t)cam + 0x68);
    float w = *(float*)((uintptr_t)cam + 0x6C);
    return {
        2.0f * (x*z + w*y),
        2.0f * (y*z - w*x),
        1.0f - 2.0f * (x*x + y*y),
        0.0f
    };
}

// ======== Plugin lifecycle ========

void FreeJumpPlugin::OnBeforeActivate()
{
    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction*
    {
        return static_cast<FreeJumpPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.ChooseAfterSorting_fnp = nullptr;
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 5.0f;
    m_Callbacks.m_Name = "FreeJumpPlugin";

    LoadSettings();
    LOG_DEBUG(DefaultLogger, "[FreeJump] Activated. Bound key: 0x%X\n", m_ToggleKey);
}

void FreeJumpPlugin::OnBeforeDeactivate()
{
    if (m_HookActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActive = false;
    }
    SaveSettings();
    LOG_DEBUG(DefaultLogger, "[FreeJump] Deactivated.\n");
}

void FreeJumpPlugin::OnUpdate()
{
    // ---------- Rebind free‑jump key ----------
    if (m_WaitingForKey) {
        for (int vk = 1; vk <= 0xFE; ++vk) {
            if (IsRisingEdgePressed(vk)) {
                m_ToggleKey = vk;
                m_WaitingForKey = false;
                LOG_DEBUG(DefaultLogger, "FreeJump key rebound to: 0x%02X\n", m_ToggleKey);
                SaveSettings();
                break;
            }
        }
    }

    // ---------- Rebind center‑screen key ----------
    if (m_WaitingForCenterKey) {
        for (int vk = 1; vk <= 0xFE; ++vk) {
            if (IsRisingEdgePressed(vk)) {
                m_CenterScreenKey = vk;
                m_WaitingForCenterKey = false;
                LOG_DEBUG(DefaultLogger, "CenterScreen key rebound to: 0x%02X\n", m_CenterScreenKey);
                break;
            }
        }
    }

    // ---------- Toggle center‑screen mode ----------
    static bool prevCenterKeyState = false;
    bool currCenterKeyState = (GetAsyncKeyState(m_CenterScreenKey) & 1) != 0;
    if (currCenterKeyState && !prevCenterKeyState) {
        m_CenterScreenMode = !m_CenterScreenMode;
        LOG_DEBUG(DefaultLogger, "CenterScreen mode %s\n", m_CenterScreenMode ? "ENABLED" : "DISABLED");
    }
    prevCenterKeyState = currCenterKeyState;

    // ---------- Dynamically activate/deactivate the parkour hook ----------
    bool keyHeld = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    if (keyHeld && !m_HookActive)
    {
        // Turn hook ON
        auto gph = GenericHooksInParkourFiltering::GetSingleton();
        gph->Subscribe(m_Callbacks);
        m_HookActivator = gph->RequestGPHSortAndSelect();
        m_HookActive = true;
        LOG_DEBUG(DefaultLogger, "[FreeJump] Hook activated.\n");
    }
    else if (!keyHeld && m_HookActive)
    {
        // Turn hook OFF
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActive = false;
        LOG_DEBUG(DefaultLogger, "[FreeJump] Hook deactivated.\n");
    }
}

void FreeJumpPlugin::OnImGuiRender()
{
    bool freeMode = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    const ImVec4 freeOnColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 freeOffColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("FreeJumpPlugin");
    ImGui::Text("Free Jump:");
    ImGui::SameLine();
    ImGui::TextColored(freeMode ? freeOnColor : freeOffColor, freeMode ? "ACTIVE (HOLD)" : "OFF");
    ImGui::Text("  Hold key for free jump");
    ImGui::Text("  Bound key: 0x%02X", m_ToggleKey);
    if (m_WaitingForKey) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.20f, 1.00f), "Press any key...");
    }
    if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind")) {
        m_WaitingForKey = !m_WaitingForKey;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click Rebind, then press the key you want to use.");

    ImGui::Separator();

    bool centerMode = m_CenterScreenMode;
    const ImVec4 centerOnColor(0.20f, 0.60f, 0.90f, 1.00f);
    const ImVec4 centerOffColor(0.70f, 0.70f, 0.70f, 1.00f);

    ImGui::Text("Center Screen:");
    ImGui::SameLine();
    ImGui::TextColored(centerMode ? centerOnColor : centerOffColor, centerMode ? "ON" : "OFF");
    ImGui::Text("  Toggle key: 0x%02X", m_CenterScreenKey);
    if (m_WaitingForCenterKey) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.20f, 1.00f), "Press any key...");
    }
    if (ImGui::Button(m_WaitingForCenterKey ? "Cancel Rebind" : "Rebind##Center")) {
        m_WaitingForCenterKey = !m_WaitingForCenterKey;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click Rebind, then press the key you want to use.");

    ImGui::Text("  Dot threshold:");
    ImGui::SameLine();
    if (ImGui::SliderFloat("##Thresh", &m_CenterScreenThreshold, 0.5f, 1.0f, "%.2f"))
        SaveSettings();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lower = wider cone. 1.0 = dead center, 0.85 = ~32 degree cone.");
}

AvailableParkourAction* FreeJumpPlugin::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    // 1. FREE‑JUMP MODE (hold key) – clears ALL actions
    if (GetAsyncKeyState(m_ToggleKey) & 0x8000) {
        actions.size = 0;
        return nullptr;
    }

    // 2. CENTER SCREEN MODE (toggle) – only allow actions whose destination is near screen center
    if (m_CenterScreenMode) {
        Vector4f camPos = GetCameraPosition();
        Vector4f camForward = GetCameraForward3D();

        for (int i = 0; i < actions.size; ++i) {
            AvailableParkourAction* action = actions[i];
            if (!action) continue;
            Vector4f* destPos = (Vector4f*)((uintptr_t)action + 0x30);
            float dx = destPos->x - camPos.x;
            float dy = destPos->y - camPos.y;
            float dz = destPos->z - camPos.z;
            float len = sqrtf(dx*dx + dy*dy + dz*dz);
            if (len < 0.001f) continue;
            float dot = (dx/len) * camForward.x + (dy/len) * camForward.y + (dz/len) * camForward.z;
            if (dot < m_CenterScreenThreshold) {
                float* fitness = (float*)((uintptr_t)action + 0x204);
                *fitness = 0.0f;
            }
        }
    }

    return nullptr;
}

bool FreeJumpPlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}