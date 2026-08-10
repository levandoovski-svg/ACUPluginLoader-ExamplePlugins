#include "pch.h"

#include "ParkourCameraPlugin.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/InputContainer.h"
#include <fstream>
#include <sstream>
#include <Windows.h>
#include <shlobj.h>
#include <cstdio>

// ======== Camera helpers ========

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

static void GetCameraYawPitch(float* outYaw, float* outPitch)
{
    Vector4f fwd = GetCameraForward3D();
    *outYaw = atan2f(fwd.x, fwd.z);
    *outPitch = asinf(fwd.y);
}

// ======== Settings persistence ========

static std::string GetGameSaveFolder()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        std::string path(documents);
        path += "\\Assassin's Creed Unity\\ParkourCameraPlugin.ini";
        return path;
    }
    return "ParkourCameraPlugin.ini";
}

void ParkourCameraPlugin::LoadSettings()
{
    std::ifstream file(GetGameSaveFolder());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("FreeJumpKey=", 0) == 0)
            try { m_ToggleKey = std::stoi(line.substr(13), nullptr, 16); } catch (...) {}
        else if (line.rfind("CenterScreenKey=", 0) == 0)
            try { m_CenterScreenKey = std::stoi(line.substr(16), nullptr, 16); } catch (...) {}
        else if (line.rfind("CenterScreenMode=", 0) == 0)
            m_CenterScreenMode = (line.substr(17) == "1");
        else if (line.rfind("ShowCrosshair=", 0) == 0)
            m_ShowCrosshair = (line.substr(14) == "1");
    }
}

void ParkourCameraPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetGameSaveFolder());
        if (file)
            file << "FreeJumpKey=" << std::hex << m_ToggleKey << std::dec << "\n"
                 << "CenterScreenKey=" << std::hex << m_CenterScreenKey << std::dec << "\n"
                 << "CenterScreenMode=" << (m_CenterScreenMode ? 1 : 0) << "\n"
                 << "ShowCrosshair=" << (m_ShowCrosshair ? 1 : 0) << "\n";
    } catch (...) {}
}

// ======== Plugin lifecycle ========

void ParkourCameraPlugin::OnBeforeActivate()
{
    LoadSettings();
    OutputDebugStringA("[ParkourCamera] Activated.\n");
}

void ParkourCameraPlugin::OnBeforeDeactivate()
{
    SaveSettings();
    OutputDebugStringA("[ParkourCamera] Deactivated.\n");
}

void ParkourCameraPlugin::OnUpdate()
{
    if (m_WaitingForKey) {
        for (int vk = 1; vk <= 0xFE; ++vk) {
            if (IsRisingEdgePressed(vk)) {
                m_ToggleKey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
    }

    if (m_WaitingForCenterKey) {
        for (int vk = 1; vk <= 0xFE; ++vk) {
            if (IsRisingEdgePressed(vk)) {
                m_CenterScreenKey = vk;
                m_WaitingForCenterKey = false;
                char buf[64]; snprintf(buf, sizeof(buf), "CenterScreen key rebound to: 0x%02X\n", m_CenterScreenKey); OutputDebugStringA(buf);
                break;
            }
        }
    }

    static bool prevCenterKeyState = false;
    bool currCenterKeyState = (GetAsyncKeyState(m_CenterScreenKey) & 1) != 0;
    if (currCenterKeyState && !prevCenterKeyState) {
        m_CenterScreenMode = !m_CenterScreenMode;
        OutputDebugStringA(m_CenterScreenMode ? "CenterScreen mode ENABLED\n" : "CenterScreen mode DISABLED\n");
    }
    prevCenterKeyState = currCenterKeyState;

    bool active = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0 || m_CenterScreenMode;

    if (active) {
        Vector4f fwd = GetCameraForward3D();

        // Project camera forward onto the horizontal plane and use as WASD direction.
        // wasdVector.x = left/right (positive = D/right)
        // wasdVector.y = forward/back (positive = W/forward)
        // When looking up/down, horizontal magnitude shrinks naturally → less movement.

        auto* hasInput = HasInputContainers::GetSingleton();
        if (!hasInput) return;
        auto* p10 = hasInput->p_10;
        if (!p10) return;
        auto* input = p10->inputContainer;
        if (!input) return;

        input->wasdVector.x = fwd.x;
        input->wasdVector.y = fwd.z;

        // Climbing/beam system may read from InputContainerBig instead
        auto* inputBig = p10->inputContainerBig;
        if (inputBig) {
            inputBig->wasdVector.x = fwd.x;
            inputBig->wasdVector.y = fwd.z;
        }
    }
}

void ParkourCameraPlugin::OnOverlayRender()
{
    if (!m_ShowCrosshair) return;

    auto* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) return;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImVec2 center(displaySize.x * 0.5f, displaySize.y * 0.5f);

    ImU32 color = m_CenterScreenMode ? IM_COL32(0, 255, 100, 220) : IM_COL32(200, 200, 200, 160);
    float len = 14.0f;
    float gap = 3.0f;

    drawList->AddLine(ImVec2(center.x - len, center.y), ImVec2(center.x - gap, center.y), color, 1.5f);
    drawList->AddLine(ImVec2(center.x + gap, center.y), ImVec2(center.x + len, center.y), color, 1.5f);
    drawList->AddLine(ImVec2(center.x, center.y - len), ImVec2(center.x, center.y - gap), color, 1.5f);
    drawList->AddLine(ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + len), color, 1.5f);

    drawList->AddCircle(center, 2.0f, color, 0, 1.5f);
}

void ParkourCameraPlugin::OnImGuiRender()
{
    ImGui::Text("ParkourCameraPlugin");

    bool freeMode = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    const ImVec4 freeOnColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 freeOffColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::SeparatorText("Camera-Directed Movement");
    {
        ImGui::Text("Hold key state:");
        ImGui::SameLine();
        ImGui::TextColored(freeMode ? freeOnColor : freeOffColor, freeMode ? "ACTIVE" : "OFF");
        ImGui::Text("Hold key: 0x%02X", m_ToggleKey);
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

        bool centerMode = m_CenterScreenMode;
        const ImVec4 centerOnColor(0.20f, 0.60f, 0.90f, 1.00f);
        const ImVec4 centerOffColor(0.70f, 0.70f, 0.70f, 1.00f);

        ImGui::Text("Toggle state:");
        ImGui::SameLine();
        ImGui::TextColored(centerMode ? centerOnColor : centerOffColor, centerMode ? "ON" : "OFF");
        ImGui::Text("Toggle key: 0x%02X", m_CenterScreenKey);
        if (m_WaitingForCenterKey) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.20f, 1.00f), "Press any key...");
        }
        if (ImGui::Button(m_WaitingForCenterKey ? "Cancel Rebind" : "Rebind##Center")) {
            m_WaitingForCenterKey = !m_WaitingForCenterKey;
        }

        ImGui::Text("Movement follows camera yaw/pitch.");
        ImGui::Text("Looking up/down reduces horizontal speed.");
    }

    ImGui::SeparatorText("Overlay");
    {
        if (ImGui::Checkbox("Show crosshair", &m_ShowCrosshair))
            SaveSettings();
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Shows a crosshair at screen center. Green = camera-directed mode on.");
    }

    {
        Vector4f fwd = GetCameraForward3D();
        float pitch = asinf(fwd.y);
        float yaw = atan2f(fwd.x, fwd.z);
        Vector4f pos = GetCameraPosition();
        ImGui::SeparatorText("Debug");
        ImGui::Text("Camera pos: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
        ImGui::Text("Camera fwd: %.2f, %.2f, %.2f", fwd.x, fwd.y, fwd.z);
        ImGui::Text("Yaw: %.2f rad (%.1f deg)", yaw, yaw * 180.0f / 3.14159f);
        ImGui::Text("Pitch: %.2f rad (%.1f deg)", pitch, pitch * 180.0f / 3.14159f);

        auto* hasInput = HasInputContainers::GetSingleton();
        if (hasInput && hasInput->p_10 && hasInput->p_10->inputContainer) {
            auto& wv = hasInput->p_10->inputContainer->wasdVector;
            ImGui::Text("WASD vector: %.2f, %.2f", wv.x, wv.y);
        }
    }
}

bool ParkourCameraPlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}
