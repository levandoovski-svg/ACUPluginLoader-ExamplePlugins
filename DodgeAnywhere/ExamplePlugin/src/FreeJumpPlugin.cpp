#include "pch.h"

#include "FreeJumpPlugin.h"
#include "MyLog.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include <fstream>
#include <sstream>
#include <Windows.h>
#include <shlobj.h>
#include <cmath>

// ============================================================
//  Settings persistence
// ============================================================

static std::string GetGameSaveFolder()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        std::string path(documents);
        path += "\\Assassin's Creed Unity\\FreeJumpPlugin.ini";
        return path;
    }
    return "FreeJumpPlugin.ini";
}

void FreeJumpPlugin::LoadSettings()
{
    std::ifstream file(GetGameSaveFolder());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("SideEjectKey=", 0) == 0)
            try { m_ToggleKey = std::stoi(line.substr(12), nullptr, 16); } catch (...) {}
        else if (line.rfind("SideEject=", 0) == 0)
            m_SideEjectEnabled = (line.substr(10) == "1");
    }
}

void FreeJumpPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetGameSaveFolder());
        if (file)
        {
            file << "SideEjectKey=" << std::hex << m_ToggleKey << std::dec << "\n";
            file << "SideEject="   << (m_SideEjectEnabled ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

// ============================================================
//  Camera-aligned facing
//  Computes a flat XY direction from Arno to the camera,
//  then snaps his forward/right to that direction.
//  Camera elevation has zero effect — only horizontal
//  position difference matters.
// ============================================================

void FreeJumpPlugin::AlignPlayerFacingToCamera()
{
    Entity* player = ACU::GetPlayer();
    if (!player) return;

    auto* cam = ACU::GetPlayerCameraComponent();
    if (!cam) return;

    const Vector3f playerPos = player->GetPosition();
    const Vector3f cameraPos = (Vector3f&)cam->positionLookFrom;

    // Flat horizontal direction from player to camera
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

// ============================================================
//  Plugin lifecycle -- virtual overrides
// ============================================================

uint64 FreeJumpPlugin::GetThisPluginVersion()
{
    return 0x00000100;
}

bool FreeJumpPlugin::InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader)
{
    LoadSettings();
    LOG_DEBUG(DefaultLogger, "[SideEject] Activated. Arm key: 0x%X\n", m_ToggleKey);
    return true;
}

void FreeJumpPlugin::EveryFrameWhenMenuIsOpen()
{
    bool keyHeld = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;

    const ImVec4 onColor (0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 offColor(1.00f, 0.55f, 0.10f, 1.00f);

    ImGui::Text("Camera-Directed Eject Plugin");
    ImGui::Text("Arm Key:");
    ImGui::SameLine();
    ImGui::TextColored(keyHeld ? onColor : offColor, keyHeld ? "HELD" : "NOT HELD");
    ImGui::Text("  Bound key: 0x%02X", m_ToggleKey);
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
            "If you bind Space, a single press rotates and ejects.\n"
            "Otherwise hold the arm key + press Space."
        );

    ImGui::Separator();

    if (ImGui::Checkbox("Camera-Directed Eject", &m_SideEjectEnabled))
        SaveSettings();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Press the arm key + Space to snap Arno's facing\n"
            "to the camera direction. His back eject will then\n"
            "launch him opposite the camera = toward your target."
        );
}

void FreeJumpPlugin::EveryFrameEvenWhenMenuIsClosed()
{
    // ----------------------------------------------------------------
    // Rebind arm key
    // ----------------------------------------------------------------
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if ((GetAsyncKeyState(vk) & 1) != 0)
            {
                m_ToggleKey     = vk;
                m_WaitingForKey = false;
                LOG_DEBUG(DefaultLogger, "[SideEject] Arm key rebound to: 0x%02X\n", m_ToggleKey);
                SaveSettings();
                break;
            }
        }
    }

    // ----------------------------------------------------------------
    // Combo trigger: rotation + eject on arm key & space
    //   - If arm key == Space: single-button mode (space press triggers)
    //   - Otherwise: hold arm key + press Space triggers
    // ----------------------------------------------------------------
    bool keyHeld   = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    bool spaceDown = (GetAsyncKeyState(VK_SPACE)      & 0x8000) != 0;
    bool spaceRising = spaceDown && !m_PrevSpaceDown;
    m_PrevSpaceDown = spaceDown;

    if (m_SideEjectEnabled)
    {
        bool trigger = (m_ToggleKey == VK_SPACE) ? spaceRising : (keyHeld && spaceRising);
        if (trigger)
            AlignPlayerFacingToCamera();
    }
}

FreeJumpPlugin g_Plugin;
