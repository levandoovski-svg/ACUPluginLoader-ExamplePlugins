#include "pch.h"
#include "CameraFacingEjectPlugin.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/Entity.h"
#include "ACU/BaseEntity.h"

#include <shlobj.h>

void CameraFacingEjectPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("ArmKey=", 0) == 0)
            try { m_ArmKey = std::stoi(line.substr(7), nullptr, 16); } catch (...) {}
        else if (line.rfind("Enabled=", 0) == 0)
            m_PluginEnabled = (line.substr(8) == "1");
        else if (line.rfind("Delay=", 0) == 0)
            try { m_DelaySeconds = std::stof(line.substr(6)); } catch (...) {}
    }
}

void CameraFacingEjectPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetIniPath());
        if (file)
        {
            file << "ArmKey=" << std::hex << m_ArmKey << std::dec << "\n";
            file << "Enabled=" << (m_PluginEnabled ? 1 : 0) << "\n";
            file << "Delay=" << m_DelaySeconds << "\n";
        }
    } catch (...) {}
}

std::string CameraFacingEjectPlugin::GetIniPath()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        return std::string(documents) + "\\Assassin's Creed Unity\\CameraFacingEject.ini";
    }
    return "CameraFacingEject.ini";
}

void CameraFacingEjectPlugin::AlignPlayerFacingToCamera()
{
    Entity* player = ACU::GetPlayer();
    if (!player) return;

    auto* cam = ACU::GetPlayerCameraComponent();
    if (!cam) return;

    const Vector3f playerPos = player->GetPosition();
    const Vector3f cameraPos = (Vector3f&)cam->positionLookFrom;

    float dx = cameraPos.x - playerPos.x;
    float dy = cameraPos.y - playerPos.y;
    float len = sqrtf(dx * dx + dy * dy);

    if (len < 0.0001f)
    {
        dx = 0.0f;
        dy = 1.0f;
    }
    else
    {
        dx /= len;
        dy /= len;
    }

    Vector3f& right   = (Vector3f&)player->GetTransform()[4 * 0];
    Vector3f& forward = (Vector3f&)player->GetTransform()[4 * 1];

    const float oldForwardZ = forward.z;
    const float oldRightZ   = right.z;

    forward.x = dx;
    forward.y = dy;
    forward.z = oldForwardZ;

    right.x =  dy;
    right.y = -dx;
    right.z = oldRightZ;
}

void CameraFacingEjectPlugin::OnUpdate()
{
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if ((GetAsyncKeyState(vk) & 1) != 0)
            {
                m_ArmKey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
    }

    if (!m_PluginEnabled) return;

    bool armDown   = (GetAsyncKeyState(m_ArmKey)  & 0x8000) != 0;
    bool spaceDown = (GetAsyncKeyState(VK_SPACE)   & 0x8000) != 0;
    bool spaceRising = spaceDown && !m_PrevSpaceDown;
    m_PrevSpaceDown = spaceDown;

    bool trigger = (m_ArmKey == VK_SPACE) ? spaceRising : (armDown && spaceRising);
    if (trigger)
    {
        m_PendingRotation = true;
        m_PendingStartTick = GetTickCount64();
    }

    if (m_PendingRotation)
    {
        bool armDown_ = (m_ArmKey == VK_SPACE) || (GetAsyncKeyState(m_ArmKey) & 0x8000) != 0;
        if (!armDown_)
        {
            m_PendingRotation = false;
        }
        else
        {
            uint64 elapsed = GetTickCount64() - m_PendingStartTick;
            if (elapsed >= (uint64)(m_DelaySeconds * 1000.0f))
            {
                AlignPlayerFacingToCamera();
                m_PendingRotation = false;
            }
        }
    }
}

void CameraFacingEjectPlugin::OnImGuiRender()
{
    bool keyHeld = (GetAsyncKeyState(m_ArmKey) & 0x8000) != 0;

    const ImVec4 onColor (0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 offColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("Camera-Facing Eject Plugin");
    ImGui::Text("Arm Key: ");
    ImGui::SameLine();
    ImGui::TextColored(keyHeld ? onColor : offColor, keyHeld ? "HELD" : "NOT HELD");
    ImGui::Text("  Bound key: 0x%02X", m_ArmKey);
    if (m_WaitingForKey)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f), "Press any key...");
    }
    if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind"))
        m_WaitingForKey = !m_WaitingForKey;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Hold arm key then press Space to snap Arno's facing\n"
            "to the camera direction. His back/side eject will then\n"
            "launch him toward where the camera is pointing.\n"
            "Default arm key: Mouse Back (XBUTTON1)."
        );

    ImGui::Separator();

    if (ImGui::Checkbox("Camera-Directed Eject", &m_PluginEnabled))
        SaveSettings();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Hold the arm key + press Space to snap Arno's facing\n"
            "to the camera direction. His back/side eject will then\n"
            "launch him in that direction.\n"
            "Tip: you can bind any key via Rebind above."
        );

    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputFloat("Rotation delay (s)", &m_DelaySeconds, 0.05f, 0.5f, "%.2f"))
    {
        if (m_DelaySeconds < 0.0f) m_DelaySeconds = 0.0f;
        if (m_DelaySeconds > 10.0f) m_DelaySeconds = 10.0f;
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Delay in seconds after pressing Space\n"
            "before the facing rotation occurs.\n"
            "Set to 0.0 for instant rotation."
        );
}
