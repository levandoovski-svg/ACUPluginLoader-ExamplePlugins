#include "pch.h"
#include "PhotoModePlugin.h"

#include "ACU_DefineNativeFunction.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/InputContainer.h"
#include "ACU/World.h"
#include "ACU/Entity.h"
#include "Common_Plugins/ACU_InputUtils.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"

#include <cmath>
#include <shlobj.h>

static constexpr float PI = 3.14159265358979323846f;
static constexpr float VERTICAL_LIMIT = 1.48f;
static constexpr float MIN_FOV = 0.2f;
static constexpr float MAX_FOV = 2.0f;

// Yaw rotation keys: hold { / } to rotate left/right. Key-driven instead of
// mouse X + MMB because the game consumes the mouse deltas while MMB is held
// (the MMB approach never delivered rotation to the camera).
static constexpr int YAW_LEFT_KEY = VK_OEM_4;   // {
static constexpr int YAW_RIGHT_KEY = VK_OEM_6;  // }
static constexpr float YAW_ROTATE_SPEED = 1.0f; // radians/sec (Shift = 5x)

PhotoModePlugin* g_pPhotoMode = nullptr;

PhotoModePlugin::PhotoModePlugin()
{
    g_pPhotoMode = this;
}

// Slow-motion / freeze game function (Steam build; same address used by
// FreeCameraRotationPlugin and DisableCameraLockPlugin).
DEFINE_GAME_FUNCTION(World__SetUnpausedGameTimescale_onSlowMotion, 0x141D5E210,
    void, __fastcall, (World* world, float newTimescale));

static float BringToIntervalWithWraparound(float current, float min_, float max_)
{
    const float interval = max_ - min_;
    while (current >= max_) current -= interval;
    while (current < min_) current += interval;
    return current;
}

static float ClampFloat(float value, float min_, float max_)
{
    if (value < min_) return min_;
    if (value > max_) return max_;
    return value;
}

// Convert a camera basis (right, up, fwd — unit, pairwise orthogonal) to the
// orientation quaternion the game stores in quaternion_mb (Vector4f x,y,z,w).
// Solved from the rotation-matrix trace / off-diagonal differences.
static Vector4f QuaternionFromBasis(const Vector3f& right, const Vector3f& up, const Vector3f& fwd)
{
    const float trace = right.x + up.y + fwd.z;
    float w = sqrtf(fmaxf(0.0f, 1.0f + trace)) * 0.5f;
    if (w < 0.05f) w = 0.05f;
    const float inv = 0.25f / w;
    const float x = (up.z - fwd.y) * inv;
    const float y = (fwd.x - right.z) * inv;
    const float z = (right.y - up.x) * inv;
    return Vector4f(x, y, z, w);
}

// Basis for the free-camera look direction. Uses IDENTICAL yaw/pitch math to
// the forward vector used for movement and look-at, so pose and orientation
// never disagree (the old code wrote pose but left the game's own orientation
// untouched — that mismatch is what made the camera shudder).
static Vector4f BuildFreeCameraQuaternion(float yaw, float pitch)
{
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw), sy = sinf(yaw);

    Vector3f fwd(sy * cp, sp, cy * cp);
    Vector3f right(cy, 0.0f, -sy);
    Vector3f up(
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x);
    return QuaternionFromBasis(right, up, fwd);
}

// Level-horizon orientation for the free camera — removed (v1.4): the game
// reads quaternion_mb as the camera's roll around the look-at axis, so any
// quat we write produces the tilted look. We now embrace that: the tilt look
// IS the freecam look, and left/right rotation is a held-key action instead.

// Basis for "eye looks toward target" — removed with Cinematic mode (the free
// camera drives orientation via the game's spinaround solver instead).

// ============================================================
// Camera hook: 0x141F3FE3B ("when setting FOV for frame").
// ACUFixes FreezeFOV hooks the same site with executeStolenBytes=false.
// r14 = ACUPlayerCameraComponent*. Our callback runs, then the stolen
// FOV-store opcode is SKIPPED because we write our own FOV (and pose).
// This is a write-AFTER-solve override point: the game has already solved
// the camera this frame, so whatever we write here is what the renderer
// consumes. We never touch the camera selector / mode graph.
// ============================================================
struct PhotoModeCameraHook : AutoAssemblerCodeHolder_Base
{
    PhotoModeCameraHook()
    {
        const uintptr_t whenSettingFOVforFrame = 0x141F3FE3B;
        const bool executeStolenBytes = false; // We supply the FOV ourselves.
        PresetScript_CCodeInTheMiddle(whenSettingFOVforFrame, 6,
            [](AllRegisters* params)
            {
                PhotoModePlugin* photoMode = g_pPhotoMode;
                if (!photoMode) return;

                ++photoMode->m_HookHitCount;
                photoMode->m_HookAlive = true;

                if (!photoMode->ShouldOverrideFrame()) return;

                auto* cam = (ACUPlayerCameraComponent*)params->r14_;
                if (!cam) return;

                __try
                {
                    switch (photoMode->GetMode())
                    {
                    case PhotoModePlugin::Mode::Free:      photoMode->ApplyFreeCamera(cam); break;
                    default: break;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            },
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, executeStolenBytes);
    }

    void OnBeforeDeactivate() override
    {
        // Also restores timescale / player visibility on plugin unload.
        if (g_pPhotoMode) g_pPhotoMode->ExitMode();
    }
};

static std::unique_ptr<AutoAssembleWrapper<PhotoModeCameraHook>> g_PhotoModeHook;

// ============================================================
// Camera state application (called from the hook, inside the game's
// camera update — after the game solved the camera this frame).
// ============================================================
void PhotoModePlugin::ApplyFreeCamera(ACUPlayerCameraComponent* cam)
{
    const float cp = cosf(m_Pitch);
    const float sp = sinf(m_Pitch);
    const float cy = cosf(m_Yaw);
    const float sy = sinf(m_Yaw);

    // Look direction from yaw/pitch (forward = (sinY*cosP, sinP, cosY*cosP)).
    Vector3f fwd(sy * cp, sp, cy * cp);

    const Vector3f lookTarget = m_FreeCamPos + fwd;

    cam->positionLookFrom = Vector4f(m_FreeCamPos, 1.0f);
    cam->locationLookat_A90 = Vector4f(lookTarget, 1.0f);
    cam->fov_mb_pi_4 = m_Fov;
    cam->fovPrecalc = m_Fov * (PI / 4.0f);

    // Tilt look by default: write our own orientation quat so the vanilla
    // bob/shake (sprint/landing acrobatics) never reaches the freecam — the
    // game reads this quat as the camera's roll around the look-at axis, which
    // gives the tilted look, and overwriting it every frame kills the acrobatic
    // shake. While a rotate key ({ / }) is held, leave the game's own quat
    // alone: its bob/shake comes back AND yaw cleanly turns the camera
    // left/right (with our quat in control, yaw only rolls the horizon).
    const bool rotating = (GetAsyncKeyState(YAW_LEFT_KEY) & 0x8000) != 0 ||
                          (GetAsyncKeyState(YAW_RIGHT_KEY) & 0x8000) != 0;
    if (!rotating)
        cam->quaternion_mb = BuildFreeCameraQuaternion(m_Yaw, m_Pitch);

    // Spin the game's own mixer to our pose so its next-frame solve can't
    // fight us. Center = one unit ahead along the view ray, distance 1, so the
    // game's solver places the camera exactly at our position looking at
    // lookTarget — using ITS conventions, no quaternion math on our side.
    cam->spinaroundAngleZtarget = m_Yaw;
    cam->spinaroundAngleUpDownTarget = ClampFloat(m_Pitch, -VERTICAL_LIMIT, VERTICAL_LIMIT);
    cam->distFromSpinaround_mb = 1.0f;
    cam->locationSpinaround_AA0 = Vector4f(lookTarget, 1.0f);

    if (m_DisableSmoothing)
        cam->disableCameraSmoothingForThisFrame = 1;
}

// Cinematic follow removed (v1.2) — the free camera with Follow Player makes
// it redundant.

// ============================================================
// Settings persistence (Documents\Assassin's Creed Unity\PhotoModePlugin.ini)
// ============================================================
std::string PhotoModePlugin::GetIniPath()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        std::string path(documents);
        path += "\\Assassin's Creed Unity\\PhotoModePlugin.ini";
        return path;
    }
    return "PhotoModePlugin.ini";
}

void PhotoModePlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("FreeCamKey=", 0) == 0)
            try { m_FreeCamKey = std::stoi(line.substr(11), nullptr, 16); } catch (...) {}
        else if (line.rfind("ResetKey=", 0) == 0)
            try { m_ResetKey = std::stoi(line.substr(9), nullptr, 16); } catch (...) {}
        else if (line.rfind("FreezeWorld=", 0) == 0)
            m_FreezeWorld = (line.substr(12) == "1");
        else if (line.rfind("HidePlayer=", 0) == 0)
            m_HidePlayer = (line.substr(11) == "1");
        else if (line.rfind("FollowPlayer=", 0) == 0)
            m_FollowPlayer = (line.substr(13) == "1");
        else if (line.rfind("FreezeAllowLook=", 0) == 0)
            m_FreezeAllowLook = (line.substr(16) == "1");
        else if (line.rfind("FreezeCamera=", 0) == 0)
            m_FreezeCamera = (line.substr(13) == "1");
        else if (line.rfind("MoveSpeed=", 0) == 0)
            try { m_MoveSpeed = std::stof(line.substr(10)); } catch (...) {}
        else if (line.rfind("MouseSensitivity=", 0) == 0)
            try { m_MouseSensitivity = std::stof(line.substr(17)); } catch (...) {}
        else if (line.rfind("InvertY=", 0) == 0)
            m_InvertY = (line.substr(8) == "1");
        else if (line.rfind("DisableSmoothing=", 0) == 0)
            m_DisableSmoothing = (line.substr(17) == "1");
    }
}

void PhotoModePlugin::SaveSettings()
{
    try {
        std::ofstream file(GetIniPath());
        if (file)
        {
            file << "FreeCamKey=" << std::hex << m_FreeCamKey << std::dec << "\n"
                 << "ResetKey=" << std::hex << m_ResetKey << std::dec << "\n"
                 << "FreezeWorld=" << (m_FreezeWorld ? 1 : 0) << "\n"
                 << "HidePlayer=" << (m_HidePlayer ? 1 : 0) << "\n"
                 << "FollowPlayer=" << (m_FollowPlayer ? 1 : 0) << "\n"
                 << "FreezeCamera=" << (m_FreezeCamera ? 1 : 0) << "\n"
                 << "FreezeAllowLook=" << (m_FreezeAllowLook ? 1 : 0) << "\n"
                 << "MoveSpeed=" << m_MoveSpeed << "\n"
                 << "MouseSensitivity=" << m_MouseSensitivity << "\n"
                 << "InvertY=" << (m_InvertY ? 1 : 0) << "\n"
                 << "DisableSmoothing=" << (m_DisableSmoothing ? 1 : 0) << "\n";
        }
    } catch (...) {}
}

// ============================================================
// Lifecycle
// ============================================================
void PhotoModePlugin::OnBeforeActivate()
{
    LoadSettings();
    g_PhotoModeHook = std::make_unique<AutoAssembleWrapper<PhotoModeCameraHook>>();
    g_PhotoModeHook->Activate();
}

void PhotoModePlugin::OnBeforeDeactivate()
{
    ExitMode();
    if (g_PhotoModeHook)
    {
        g_PhotoModeHook->Deactivate();
        g_PhotoModeHook.reset();
    }
}

// ============================================================
// Mode helpers
// ============================================================
void PhotoModePlugin::SetWorldTimescale(float newTimescale)
{
    World* w = World::GetSingleton();
    if (w)
        World__SetUnpausedGameTimescale_onSlowMotion(w, newTimescale);
}

void PhotoModePlugin::SnapCameraFromGame()
{
    m_HasSnapshot = false;

    ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
    if (cam)
    {
        m_SnapshotPos = (Vector3f&)cam->positionLookFrom;
        m_SnapshotFov = cam->fov_mb_pi_4 > 0.2f ? cam->fov_mb_pi_4 : 1.0f;

        Vector3f look(
            cam->locationLookat_A90.x - cam->positionLookFrom.x,
            cam->locationLookat_A90.y - cam->positionLookFrom.y,
            cam->locationLookat_A90.z - cam->positionLookFrom.z);
        const float len = look.length();
        if (len > 0.0001f)
        {
            m_SnapshotYaw = atan2f(look.x, look.z);
            m_SnapshotPitch = ClampFloat(asinf(look.y / len), -VERTICAL_LIMIT, VERTICAL_LIMIT);
        }

        Entity* player = ACU::GetPlayer();
        if (player && m_SnapshotPos.x == 0.0f && m_SnapshotPos.y == 0.0f && m_SnapshotPos.z == 0.0f)
            m_SnapshotPos = player->GetPosition();

        m_HasSnapshot = true;
    }
}

void PhotoModePlugin::EnterFreeMode()
{
    if (m_Mode == Mode::Free) return;
    ExitMode(); // cleanly leaves cinematic mode if it was active

    SnapCameraFromGame();
    m_FreeCamPos = m_SnapshotPos;
    m_Yaw = m_SnapshotYaw;
    m_Pitch = m_SnapshotPitch;
    m_Fov = m_SnapshotFov;

    if (m_FollowPlayer)
    {
        Entity* player = ACU::GetPlayer();
        if (player)
            m_FollowOffset = m_FreeCamPos - player->GetPosition();
        // Follow Player keeps the world live so Arno stays controllable.
        m_FreezeWorld = false;
    }

    m_Mode = Mode::Free;
    m_LastTick = GetTickCount64();
}

void PhotoModePlugin::ExitMode()
{
    if (m_Mode == Mode::None) return;

    if (m_TimescaleApplied)
    {
        SetWorldTimescale(1.0f);
        m_TimescaleApplied = false;
    }
    if (m_PlayerHidden)
        RestorePlayerVisibility();

    m_Mode = Mode::None;
}

void PhotoModePlugin::ResetCamera()
{
    if (m_HasSnapshot)
    {
        m_FreeCamPos = m_SnapshotPos;
        m_Yaw = m_SnapshotYaw;
        m_Pitch = m_SnapshotPitch;
        m_Fov = m_SnapshotFov;
        if (m_FollowPlayer && m_Mode == Mode::Free)
        {
            Entity* player = ACU::GetPlayer();
            if (player)
                m_FollowOffset = m_SnapshotPos - player->GetPosition();
        }
    }
}

void PhotoModePlugin::ApplyHidePlayer()
{
    Entity* player = ACU::GetPlayer();
    if (!player) return;
    m_SavedPlayerFlags88 = *(uint64*)&player->flags88;
    player->flags88.IsHidden = 1;
    m_PlayerHidden = true;
}

void PhotoModePlugin::RestorePlayerVisibility()
{
    Entity* player = ACU::GetPlayer();
    if (player)
        *(uint64*)&player->flags88 = m_SavedPlayerFlags88;
    m_PlayerHidden = false;
}

// ============================================================
// Per-frame input
// ============================================================
bool PhotoModePlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}

void PhotoModePlugin::UpdateFreeInput(float dt)
{
    // Timescale sync (live "Freeze World" checkbox). Follow Player / Freeze
    // Camera keep the world live so Arno can be controlled during freecam.
    const bool keepWorldLive = m_FollowPlayer || m_FreezeCamera;
    if (m_FreezeWorld && !keepWorldLive && !m_TimescaleApplied) { SetWorldTimescale(0.0f); m_TimescaleApplied = true; }
    else if ((!m_FreezeWorld || keepWorldLive) && m_TimescaleApplied) { SetWorldTimescale(1.0f); m_TimescaleApplied = false; }

    // Player visibility sync (live "Hide Player" checkbox).
    if (m_HidePlayer && !m_PlayerHidden) ApplyHidePlayer();
    else if (!m_HidePlayer && m_PlayerHidden) RestorePlayerVisibility();

    // Freeze Camera: lock the pose in place. "Allow Mouse Look" re-enables
    // looking around (yaw/pitch) while the position stays frozen. Follow Player
    // takes priority (the camera then tracks Arno instead of freezing).
    const bool frozen = m_FreezeCamera && !m_FollowPlayer;
    const bool lookBlocked = m_FollowPlayer || (frozen && !m_FreezeAllowLook);

    // Look input: mouse Y = pitch (always when not blocked); yaw rotation is
    // key-driven — hold { / } to rotate left/right (mouse X + MMB was dropped:
    // the game swallows the mouse deltas while MMB is held, so rotation never
    // reached the camera). FOV wheel works unless look is blocked. Blocked
    // entirely in Follow Player, and in Freeze Camera unless "Allow Mouse
    // Look" is checked.
    if (!lookBlocked)
    {
        auto* inp = ACU::Input::Get_InputContainerBig();
        if (inp)
        {
            const int dy = inp->mouseState.mouseDeltaIntForCamera_Y;
            if (dy != 0)
            {
                const float yMult = m_InvertY ? -1.0f : 1.0f;
                m_Pitch += (float)dy * m_MouseSensitivity * yMult;
                if (m_Pitch > VERTICAL_LIMIT) m_Pitch = VERTICAL_LIMIT;
                if (m_Pitch < -VERTICAL_LIMIT) m_Pitch = -VERTICAL_LIMIT;
            }

            // Yaw: hold { / } to rotate left/right at a fixed rate.
            const bool rotLeft = (GetAsyncKeyState(YAW_LEFT_KEY) & 0x8000) != 0;
            const bool rotRight = (GetAsyncKeyState(YAW_RIGHT_KEY) & 0x8000) != 0;
            if (rotLeft || rotRight)
            {
                float yawSpeed = YAW_ROTATE_SPEED;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) yawSpeed *= 5.0f;
                m_Yaw += (rotRight ? yawSpeed : 0.0f) * dt;
                m_Yaw -= (rotLeft ? yawSpeed : 0.0f) * dt;
                m_Yaw = BringToIntervalWithWraparound(m_Yaw, -PI, PI);
            }

            const int wheel = inp->mouseState.mouseWheelDeltaInt;
            if (wheel)
                m_Fov = ClampFloat(m_Fov + (wheel > 0 ? 0.05f : -0.05f), MIN_FOV, MAX_FOV);
        }
    }

    // Follow Player: re-anchor to Arno (he may have moved since last frame)
    // BEFORE applying our own movement, using the saved offset.
    if (m_FollowPlayer)
    {
        Entity* player = ACU::GetPlayer();
        if (player)
            m_FreeCamPos = player->GetPosition() + m_FollowOffset;
    }

    // Position movement (ignored while frozen). Arrow keys always work —
    // in Follow Player they reposition the camera relative to Arno. Q/E
    // (up/down) is disabled in Follow Player so Arno's movement keys stay free.
    if (!frozen)
    {
        float speed = m_MoveSpeed;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) speed *= 10.0f;

        const float cp = cosf(m_Pitch);
        const float fwdX = sinf(m_Yaw) * cp;
        const float fwdZ = cosf(m_Yaw) * cp;
        const float rightX = cosf(m_Yaw);
        const float rightZ = -sinf(m_Yaw);

        float moveX = 0.0f, moveY = 0.0f, moveZ = 0.0f;
        if (GetAsyncKeyState(VK_UP) & 0x8000) { moveX += fwdX; moveZ += fwdZ; }
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) { moveX -= fwdX; moveZ -= fwdZ; }
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) { moveX += rightX; moveZ += rightZ; }
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) { moveX -= rightX; moveZ -= rightZ; }
        if (!m_FollowPlayer)
        {
            if (GetAsyncKeyState('E') & 0x8000) moveY += 1.0f;
            if (GetAsyncKeyState('Q') & 0x8000) moveY -= 1.0f;
        }

        if (moveX != 0.0f || moveY != 0.0f || moveZ != 0.0f)
        {
            const float step = speed * dt;
            m_FreeCamPos.x += moveX * step;
            m_FreeCamPos.y += moveY * step;
            m_FreeCamPos.z += moveZ * step;
        }
    }

    // Follow Player: capture the new offset so the anchor next frame preserves
    // where we moved the camera relative to Arno.
    if (m_FollowPlayer)
    {
        Entity* player = ACU::GetPlayer();
        if (player)
            m_FollowOffset = m_FreeCamPos - player->GetPosition();
    }
}

// ============================================================
// Saved camera poses (slots) — save current pose, jump to a slot,
// cycle with ',' and '.'.
// ============================================================
int PhotoModePlugin::SaveCurrentCameraSlot()
{
    int slot = -1;
    for (int i = 0; i < 9; ++i)
        if (!m_Slots[i].used) { slot = i; break; }
    if (slot < 0)
        slot = (m_ActiveSlot >= 0) ? m_ActiveSlot : 0; // all full: overwrite active (or first)

    m_Slots[slot].used = true;
    m_Slots[slot].pos = m_FreeCamPos;
    m_Slots[slot].yaw = m_Yaw;
    m_Slots[slot].pitch = m_Pitch;
    m_Slots[slot].fov = m_Fov;
    m_ActiveSlot = slot;
    return slot;
}

void PhotoModePlugin::ApplyCameraSlot(int index)
{
    if (index < 0 || index >= 9 || !m_Slots[index].used) return;
    m_FreeCamPos = m_Slots[index].pos;
    m_Yaw = m_Slots[index].yaw;
    m_Pitch = m_Slots[index].pitch;
    m_Fov = m_Slots[index].fov;
    m_ActiveSlot = index;
    if (m_FollowPlayer && m_Mode == Mode::Free)
    {
        Entity* player = ACU::GetPlayer();
        if (player)
            m_FollowOffset = m_FreeCamPos - player->GetPosition();
    }
}

void PhotoModePlugin::CycleCameraSlots(int dir)
{
    if (dir == 0) return;
    int i = m_ActiveSlot;
    for (int step = 0; step < 9; ++step)
    {
        i = (i + dir + 9) % 9;
        if (m_Slots[i].used)
        {
            ApplyCameraSlot(i);
            return;
        }
    }
}

void PhotoModePlugin::OnUpdate()
{
    if (!m_Enabled)
    {
        ExitMode();
        return;
    }

    // Key rebinding wait (mirrors ParkourCameraPlugin).
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (IsRisingEdgePressed(vk))
            {
                if (m_RebindTarget == 1) m_FreeCamKey = vk;
                else if (m_RebindTarget == 3) m_ResetKey = vk;
                m_WaitingForKey = false;
                m_RebindTarget = 0;
                SaveSettings();
                break;
            }
        }
    }

    const bool freeDown = (GetAsyncKeyState(m_FreeCamKey) & 0x8000) != 0;
    const bool resetDown = (GetAsyncKeyState(m_ResetKey) & 0x8000) != 0;

    if (freeDown && !m_PrevFreeDown)
    {
        if (m_Mode == Mode::Free) ExitMode();
        else EnterFreeMode();
    }
    if (resetDown && !m_PrevResetDown && m_Mode != Mode::None)
        ResetCamera();

    m_PrevFreeDown = freeDown;
    m_PrevResetDown = resetDown;

    // Saved-camera switching: ',' = previous slot, '.' = next slot.
    const bool commaDown = (GetAsyncKeyState(VK_OEM_COMMA) & 0x8000) != 0;
    const bool periodDown = (GetAsyncKeyState(VK_OEM_PERIOD) & 0x8000) != 0;
    if (commaDown && !m_PrevCommaDown && m_Mode != Mode::None) CycleCameraSlots(-1);
    if (periodDown && !m_PrevPeriodDown && m_Mode != Mode::None) CycleCameraSlots(1);
    m_PrevCommaDown = commaDown;
    m_PrevPeriodDown = periodDown;

    if (m_Mode == Mode::None) return;

    uint64 now = GetTickCount64();
    float dt = (float)(now - m_LastTick) / 1000.0f;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    m_LastTick = now;

    if (m_Mode == Mode::Free) UpdateFreeInput(dt);
}

// ============================================================
// ImGui panel
// ============================================================
void PhotoModePlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Photo Mode"))
        return;

    if (ImGui::Checkbox("Enable Photo Mode", &m_Enabled))
        if (!m_Enabled) ExitMode();

    const char* modeName = (m_Mode == Mode::Free) ? "FREE CAMERA" : "OFF";
    const ImVec4 modeColor =
        (m_Mode == Mode::None) ? ImVec4(0.7f, 0.7f, 0.7f, 1.0f) : ImVec4(0.20f, 0.90f, 0.30f, 1.00f);
    ImGui::TextColored(modeColor, "Mode: %s", modeName);

    if (!m_Enabled) return;

    ImGui::Indent();

    ImGui::Text("Free Cam:");
    ImGui::SameLine();
    ImGui::Text("0x%02X", m_FreeCamKey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey && m_RebindTarget == 1 ? "Press any key..." : "Rebind##Free"))
    {
        m_WaitingForKey = true;
        m_RebindTarget = 1;
    }

    ImGui::Text("Reset Cam:");
    ImGui::SameLine();
    ImGui::Text("0x%02X", m_ResetKey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey && m_RebindTarget == 3 ? "Press any key..." : "Rebind##Reset"))
    {
        m_WaitingForKey = true;
        m_RebindTarget = 3;
    }

    ImGui::Separator();

    ImGui::TextDisabled("CONTROLS");
    ImGui::BulletText("F9: toggle free camera");
    ImGui::BulletText("F11: reset camera to entry pose");
    ImGui::BulletText("Mouse Y: look up / down");
    ImGui::BulletText("Hold { / }: rotate left / right (Shift = faster)");
    ImGui::BulletText("Mouse wheel: FOV zoom");
    ImGui::BulletText("Arrow keys: move camera (forward/back/left/right)");
    ImGui::BulletText("Q / E: move camera up / down (disabled in Follow Player)");
    ImGui::BulletText("Shift: hold for 10x movement speed");
    ImGui::BulletText(", / .: switch between saved camera poses");
    ImGui::BulletText("Follow Player on: mouse & Q/E locked, arrows still move");
    ImGui::BulletText("Freeze Camera on: camera locked (Allow Mouse Look re-enables looking)");

    ImGui::Separator();

    ImGui::TextDisabled("FREE CAMERA (F9)");
    if (m_FollowPlayer || m_FreezeCamera)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Freeze World", &m_FreezeWorld)) SaveSettings();
    if (m_FollowPlayer || m_FreezeCamera)
    {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Disabled while Follow Player / Freeze Camera is on (the world must stay live so Arno can move).");
    }
    if (ImGui::Checkbox("Hide Player", &m_HidePlayer)) SaveSettings();
    if (ImGui::Checkbox("Follow Player", &m_FollowPlayer))
    {
        if (m_FollowPlayer) m_FreezeWorld = false;
        SaveSettings();
    }
    if (ImGui::Checkbox("Freeze Camera", &m_FreezeCamera))
    {
        if (m_FreezeCamera) m_FreezeWorld = false;
        SaveSettings();
    }
    if (m_FreezeCamera)
    {
        ImGui::Indent();
        if (ImGui::Checkbox("Allow Mouse Look", &m_FreezeAllowLook)) SaveSettings();
        ImGui::TextDisabled("Position stays frozen; mouse can still look around.");
        ImGui::Unindent();
    }
    if (m_FollowPlayer)
    {
        ImGui::Indent();
        ImGui::TextDisabled("Camera tracks Arno as he runs; world stays live.");
        ImGui::Unindent();
    }
    if (ImGui::SliderFloat("Move Speed", &m_MoveSpeed, 0.5f, 20.0f, "%.1f")) SaveSettings();

    ImGui::Separator();

    if (ImGui::SliderFloat("FOV", &m_Fov, MIN_FOV, MAX_FOV, "%.2f")) SaveSettings();
    if (ImGui::Checkbox("Invert Y", &m_InvertY)) SaveSettings();
    if (ImGui::Checkbox("Disable Camera Smoothing", &m_DisableSmoothing)) SaveSettings();

    ImGui::Separator();

    ImGui::TextDisabled("SAVED CAMERAS");
    if (ImGui::Button("Save Current Pose"))
        SaveCurrentCameraSlot();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stores the current position/angle/FOV into a free slot (overwrites the active slot when full).");
    ImGui::SameLine();
    if (ImGui::Button("Clear All"))
    {
        for (int i = 0; i < 9; ++i) m_Slots[i].used = false;
        m_ActiveSlot = -1;
    }
    for (int i = 0; i < 9; ++i)
    {
        if (!m_Slots[i].used) continue;
        char label[32];
        sprintf_s(label, "Slot %d%s", i + 1, (m_ActiveSlot == i) ? " *" : "");
        if (ImGui::Button(label))
            ApplyCameraSlot(i);
        ImGui::SameLine();
        char clearLabel[32];
        sprintf_s(clearLabel, "Clear##%d", i);
        if (ImGui::Button(clearLabel))
        {
            m_Slots[i].used = false;
            if (m_ActiveSlot == i) m_ActiveSlot = -1;
        }
    }
    ImGui::TextDisabled("Switch between saved poses with ',' and '.'");

    ImGui::Separator();

    if (ImGui::Button("RESET CAMERA", ImVec2(-1.0f, 0.0f)))
        ResetCamera();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Restores the camera to where the game camera\n"
            "was when you entered the current mode.\n"
            "Hotkey: Reset Cam (F11).");

    ImGui::Separator();

    ImGui::TextDisabled("Hook hits: %llu (%s)",
        m_HookHitCount, m_HookAlive ? "address live" : "NOT HIT - address may be wrong");

    __try {
        ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
        Entity* player = ACU::GetPlayer();
        if (cam)
        {
            ImGui::Text("Cam pos: %.1f, %.1f, %.1f",
                cam->positionLookFrom.x, cam->positionLookFrom.y, cam->positionLookFrom.z);
            ImGui::Text("FOV: %.2f (ours: %.2f)", cam->fov_mb_pi_4, m_Fov);
        }
        if (player)
        {
            Vector3f p = player->GetPosition();
            ImGui::Text("Player pos: %.1f, %.1f, %.1f", p.x, p.y, p.z);
        }
        ImGui::Text("Yaw: %.2f  Pitch: %.2f  FOV: %.2f", m_Yaw, m_Pitch, m_Fov);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    ImGui::Unindent();
}