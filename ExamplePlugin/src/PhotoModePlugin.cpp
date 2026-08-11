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
static constexpr float MIN_DISTANCE = 0.5f;
static constexpr float MAX_DISTANCE = 30.0f;

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

// Basis for "eye looks toward target" (cinematic follow looks INWARD at the
// player — the orientation must encode the actual view ray, not the orbit
// angle, or the game's orientation fights ours and the view shakes).
static Vector4f BuildLookAtQuaternion(const Vector3f& eye, const Vector3f& target)
{
    Vector3f fwd = target - eye;
    const float len = fwd.length();
    if (len < 0.0001f) return Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
    fwd = fwd * (1.0f / len);

    const Vector3f worldUp(0.0f, 1.0f, 0.0f);
    Vector3f right(
        worldUp.y * fwd.z - worldUp.z * fwd.y,
        worldUp.z * fwd.x - worldUp.x * fwd.z,
        worldUp.x * fwd.y - worldUp.y * fwd.x);
    const float rlen = right.length();
    if (rlen < 0.0001f) right = Vector3f(1.0f, 0.0f, 0.0f);
    else right = right * (1.0f / rlen);

    Vector3f up(
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x);
    return QuaternionFromBasis(right, up, fwd);
}

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
                    case PhotoModePlugin::Mode::Cinematic: photoMode->ApplyCinematicCamera(cam); break;
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

    cam->positionLookFrom = Vector4f(m_FreeCamPos, 1.0f);
    cam->locationLookat_A90 = Vector4f(m_FreeCamPos + fwd, 1.0f);
    cam->quaternion_mb = BuildFreeCameraQuaternion(m_Yaw, m_Pitch);
    cam->fov_mb_pi_4 = m_Fov;
    cam->fovPrecalc = m_Fov * (PI / 4.0f);

    // Sync the game's spinaround mixer to our angles so its next-frame solve
    // (which runs every frame regardless of our override) can't fight us.
    cam->spinaroundAngleZtarget = m_Yaw;
    cam->spinaroundAngleUpDownTarget = ClampFloat(m_Pitch, -VERTICAL_LIMIT, VERTICAL_LIMIT);
    cam->distFromSpinaround_mb = m_Distance;
    cam->locationSpinaround_AA0 = Vector4f(m_FreeCamPos, 1.0f);

    if (m_DisableSmoothing)
        cam->disableCameraSmoothingForThisFrame = 1;
}

void PhotoModePlugin::ApplyCinematicCamera(ACUPlayerCameraComponent* cam)
{
    Entity* player = ACU::GetPlayer();
    if (!player)
    {
        // No player ref (loading etc.) — at least keep our FOV.
        cam->fov_mb_pi_4 = m_Fov;
        cam->fovPrecalc = m_Fov * (PI / 4.0f);
        return;
    }

    const Vector3f playerPos = player->GetPosition();
    const float cv = cosf(m_Pitch);

    // Orbit offset around the player (same formula as DisableCameraLockPlugin).
    Vector3f offset(
        m_Distance * cv * sinf(m_Yaw),
        m_Distance * sinf(m_Pitch),
        m_Distance * cv * cosf(m_Yaw));

    const Vector3f camPos = playerPos + offset;
    const Vector3f lookTarget = playerPos + Vector3f(0.0f, 1.5f, 0.0f);

    cam->positionLookFrom = Vector4f(camPos, 1.0f);
    cam->locationLookat_A90 = Vector4f(lookTarget, 1.0f);
    cam->quaternion_mb = BuildLookAtQuaternion(camPos, lookTarget);
    cam->fov_mb_pi_4 = m_Fov;
    cam->fovPrecalc = m_Fov * (PI / 4.0f);

    // Sync the game's spinaround mixer so its next-frame solve can't fight us.
    cam->spinaroundAngleZtarget = m_Yaw;
    cam->spinaroundAngleUpDownTarget = ClampFloat(m_Pitch, -VERTICAL_LIMIT, VERTICAL_LIMIT);
    cam->distFromSpinaround_mb = m_Distance;
    cam->locationSpinaround_AA0 = Vector4f(playerPos, 1.0f);

    if (m_DisableSmoothing)
        cam->disableCameraSmoothingForThisFrame = 1;
}

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
        else if (line.rfind("CinematicKey=", 0) == 0)
            try { m_CinematicKey = std::stoi(line.substr(13), nullptr, 16); } catch (...) {}
        else if (line.rfind("ResetKey=", 0) == 0)
            try { m_ResetKey = std::stoi(line.substr(9), nullptr, 16); } catch (...) {}
        else if (line.rfind("FreezeWorld=", 0) == 0)
            m_FreezeWorld = (line.substr(12) == "1");
        else if (line.rfind("HidePlayer=", 0) == 0)
            m_HidePlayer = (line.substr(11) == "1");
        else if (line.rfind("FollowPlayer=", 0) == 0)
            m_FollowPlayer = (line.substr(13) == "1");
        else if (line.rfind("SlowMotion=", 0) == 0)
            m_SlowMotion = (line.substr(11) == "1");
        else if (line.rfind("SlowMotionTimescale=", 0) == 0)
            try { m_SlowMotionTimescale = std::stof(line.substr(20)); } catch (...) {}
        else if (line.rfind("MoveSpeed=", 0) == 0)
            try { m_MoveSpeed = std::stof(line.substr(10)); } catch (...) {}
        else if (line.rfind("MouseSensitivity=", 0) == 0)
            try { m_MouseSensitivity = std::stof(line.substr(17)); } catch (...) {}
        else if (line.rfind("InvertX=", 0) == 0)
            m_InvertX = (line.substr(8) == "1");
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
                 << "CinematicKey=" << std::hex << m_CinematicKey << std::dec << "\n"
                 << "ResetKey=" << std::hex << m_ResetKey << std::dec << "\n"
                 << "FreezeWorld=" << (m_FreezeWorld ? 1 : 0) << "\n"
                 << "HidePlayer=" << (m_HidePlayer ? 1 : 0) << "\n"
                 << "FollowPlayer=" << (m_FollowPlayer ? 1 : 0) << "\n"
                 << "SlowMotion=" << (m_SlowMotion ? 1 : 0) << "\n"
                 << "SlowMotionTimescale=" << m_SlowMotionTimescale << "\n"
                 << "MoveSpeed=" << m_MoveSpeed << "\n"
                 << "MouseSensitivity=" << m_MouseSensitivity << "\n"
                 << "InvertX=" << (m_InvertX ? 1 : 0) << "\n"
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
        m_SnapshotSpinZ = cam->spinaroundAngleZtarget;
        m_SnapshotSpinUpDown = cam->spinaroundAngleUpDownTarget;

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
        if (player)
        {
            Vector3f playerPos = player->GetPosition();
            Vector3f diff(m_SnapshotPos.x - playerPos.x, m_SnapshotPos.y - playerPos.y, m_SnapshotPos.z - playerPos.z);
            const float dist = diff.length();
            if (dist > 0.1f)
                m_SnapshotDistance = ClampFloat(dist, MIN_DISTANCE, MAX_DISTANCE);
        }

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

void PhotoModePlugin::EnterCinematicMode()
{
    if (m_Mode == Mode::Cinematic) return;
    ExitMode();

    SnapCameraFromGame();
    m_Distance = m_SnapshotDistance;
    m_Yaw = m_SnapshotYaw;
    m_Pitch = m_SnapshotPitch;
    m_Fov = m_SnapshotFov;

    m_Mode = Mode::Cinematic;
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
        m_Distance = m_SnapshotDistance;
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
    // Timescale sync (live "Freeze World" checkbox). Follow Player keeps the
    // world live so Arno can be controlled during freecam.
    if (m_FreezeWorld && !m_FollowPlayer && !m_TimescaleApplied) { SetWorldTimescale(0.0f); m_TimescaleApplied = true; }
    else if ((!m_FreezeWorld || m_FollowPlayer) && m_TimescaleApplied) { SetWorldTimescale(1.0f); m_TimescaleApplied = false; }

    // Player visibility sync (live "Hide Player" checkbox).
    if (m_HidePlayer && !m_PlayerHidden) ApplyHidePlayer();
    else if (!m_HidePlayer && m_PlayerHidden) RestorePlayerVisibility();

    // Mouse orbit + FOV wheel.
    auto* inp = ACU::Input::Get_InputContainerBig();
    if (inp)
    {
        const int dx = inp->mouseState.mouseDeltaIntForCamera_X;
        const int dy = inp->mouseState.mouseDeltaIntForCamera_Y;
        if (dx != 0 || dy != 0)
        {
            const float xMult = m_InvertX ? 1.0f : -1.0f;
            const float yMult = m_InvertY ? -1.0f : 1.0f;
            m_Yaw += (float)dx * m_MouseSensitivity * xMult;
            m_Pitch += (float)dy * m_MouseSensitivity * yMult;
            m_Yaw = BringToIntervalWithWraparound(m_Yaw, -PI, PI);
            if (m_Pitch > VERTICAL_LIMIT) m_Pitch = VERTICAL_LIMIT;
            if (m_Pitch < -VERTICAL_LIMIT) m_Pitch = -VERTICAL_LIMIT;
        }

        const int wheel = inp->mouseState.mouseWheelDeltaInt;
        if (wheel)
            m_Fov = ClampFloat(m_Fov + (wheel > 0 ? 0.05f : -0.05f), MIN_FOV, MAX_FOV);
    }

    // Follow Player: re-anchor to Arno (he may have moved since last frame)
    // BEFORE applying our own movement, using the saved offset.
    if (m_FollowPlayer)
    {
        Entity* player = ACU::GetPlayer();
        if (player)
            m_FreeCamPos = player->GetPosition() + m_FollowOffset;
    }

    // WASD/QE fly. Movement is along the camera plane (flat forward/right),
    // independent of the game clock (world may be frozen at timescale 0).
    float speed = m_MoveSpeed;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) speed *= 10.0f;

    const float cp = cosf(m_Pitch);
    const float fwdX = sinf(m_Yaw) * cp;
    const float fwdZ = cosf(m_Yaw) * cp;
    const float rightX = cosf(m_Yaw);
    const float rightZ = -sinf(m_Yaw);

    float moveX = 0.0f, moveY = 0.0f, moveZ = 0.0f;
    if (GetAsyncKeyState('W') & 0x8000) { moveX += fwdX; moveZ += fwdZ; }
    if (GetAsyncKeyState('S') & 0x8000) { moveX -= fwdX; moveZ -= fwdZ; }
    if (GetAsyncKeyState('D') & 0x8000) { moveX += rightX; moveZ += rightZ; }
    if (GetAsyncKeyState('A') & 0x8000) { moveX -= rightX; moveZ -= rightZ; }
    if (GetAsyncKeyState('E') & 0x8000) moveY += 1.0f;
    if (GetAsyncKeyState('Q') & 0x8000) moveY -= 1.0f;

    if (moveX != 0.0f || moveY != 0.0f || moveZ != 0.0f)
    {
        const float step = speed * dt;
        m_FreeCamPos.x += moveX * step;
        m_FreeCamPos.y += moveY * step;
        m_FreeCamPos.z += moveZ * step;
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

void PhotoModePlugin::UpdateCinematicInput(float dt)
{
    (void)dt; // orbit is absolute, not speed-based

    // Timescale sync (live "Slow Motion" checkbox).
    if (m_SlowMotion && !m_TimescaleApplied) { SetWorldTimescale(m_SlowMotionTimescale); m_TimescaleApplied = true; }
    else if (!m_SlowMotion && m_TimescaleApplied) { SetWorldTimescale(1.0f); m_TimescaleApplied = false; }
    else if (m_SlowMotion && m_TimescaleApplied) { SetWorldTimescale(m_SlowMotionTimescale); } // keep slider live

    auto* inp = ACU::Input::Get_InputContainerBig();
    if (inp)
    {
        const int dx = inp->mouseState.mouseDeltaIntForCamera_X;
        const int dy = inp->mouseState.mouseDeltaIntForCamera_Y;
        if (dx != 0 || dy != 0)
        {
            const float xMult = m_InvertX ? 1.0f : -1.0f;
            const float yMult = m_InvertY ? -1.0f : 1.0f;
            m_Yaw += (float)dx * m_MouseSensitivity * xMult;
            m_Pitch += (float)dy * m_MouseSensitivity * yMult;
            m_Yaw = BringToIntervalWithWraparound(m_Yaw, -PI, PI);
            if (m_Pitch > VERTICAL_LIMIT) m_Pitch = VERTICAL_LIMIT;
            if (m_Pitch < -VERTICAL_LIMIT) m_Pitch = -VERTICAL_LIMIT;
        }

        const int wheel = inp->mouseState.mouseWheelDeltaInt;
        if (wheel)
            m_Distance = ClampFloat(m_Distance + (wheel > 0 ? 0.5f : -0.5f), MIN_DISTANCE, MAX_DISTANCE);
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
                else if (m_RebindTarget == 2) m_CinematicKey = vk;
                else if (m_RebindTarget == 3) m_ResetKey = vk;
                m_WaitingForKey = false;
                m_RebindTarget = 0;
                SaveSettings();
                break;
            }
        }
    }

    const bool freeDown = (GetAsyncKeyState(m_FreeCamKey) & 0x8000) != 0;
    const bool cinDown = (GetAsyncKeyState(m_CinematicKey) & 0x8000) != 0;
    const bool resetDown = (GetAsyncKeyState(m_ResetKey) & 0x8000) != 0;

    if (freeDown && !m_PrevFreeDown)
    {
        if (m_Mode == Mode::Free) ExitMode();
        else EnterFreeMode();
    }
    if (cinDown && !m_PrevCinematicDown)
    {
        if (m_Mode == Mode::Cinematic) ExitMode();
        else EnterCinematicMode();
    }
    if (resetDown && !m_PrevResetDown && m_Mode != Mode::None)
        ResetCamera();

    m_PrevFreeDown = freeDown;
    m_PrevCinematicDown = cinDown;
    m_PrevResetDown = resetDown;

    if (m_Mode == Mode::None) return;

    uint64 now = GetTickCount64();
    float dt = (float)(now - m_LastTick) / 1000.0f;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    m_LastTick = now;

    if (m_Mode == Mode::Free) UpdateFreeInput(dt);
    else if (m_Mode == Mode::Cinematic) UpdateCinematicInput(dt);
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

    const char* modeName =
        (m_Mode == Mode::Free) ? "FREE CAMERA" :
        (m_Mode == Mode::Cinematic) ? "CINEMATIC FOLLOW" : "OFF";
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

    ImGui::Text("Cinematic:");
    ImGui::SameLine();
    ImGui::Text("0x%02X", m_CinematicKey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey && m_RebindTarget == 2 ? "Press any key..." : "Rebind##Cinematic"))
    {
        m_WaitingForKey = true;
        m_RebindTarget = 2;
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

    ImGui::TextDisabled("FREE CAMERA (F9)");
    if (m_FollowPlayer)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Freeze World", &m_FreezeWorld)) SaveSettings();
    if (m_FollowPlayer)
    {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Disabled while Follow Player is on (the world must stay live so Arno can move).");
    }
    if (ImGui::Checkbox("Hide Player", &m_HidePlayer)) SaveSettings();
    if (ImGui::Checkbox("Follow Player", &m_FollowPlayer))
    {
        if (m_FollowPlayer) m_FreezeWorld = false;
        SaveSettings();
    }
    if (m_FollowPlayer)
    {
        ImGui::Indent();
        ImGui::TextDisabled("Camera tracks Arno as he runs; world stays live.");
        ImGui::Unindent();
    }
    if (ImGui::SliderFloat("Move Speed", &m_MoveSpeed, 0.5f, 20.0f, "%.1f")) SaveSettings();

    ImGui::Separator();

    ImGui::TextDisabled("CINEMATIC FOLLOW (F10)");
    if (ImGui::Checkbox("Slow Motion", &m_SlowMotion)) SaveSettings();
    if (m_SlowMotion)
    {
        ImGui::Indent();
        if (ImGui::SliderFloat("Timescale", &m_SlowMotionTimescale, 0.01f, 1.0f, "%.2f")) SaveSettings();
        ImGui::Unindent();
    }

    ImGui::Separator();

    if (ImGui::SliderFloat("FOV", &m_Fov, MIN_FOV, MAX_FOV, "%.2f")) SaveSettings();
    if (ImGui::SliderFloat("Orbit Distance", &m_Distance, MIN_DISTANCE, MAX_DISTANCE, "%.1f")) SaveSettings();
    if (ImGui::Checkbox("Invert X", &m_InvertX)) SaveSettings();
    if (ImGui::Checkbox("Invert Y", &m_InvertY)) SaveSettings();
    if (ImGui::Checkbox("Disable Camera Smoothing", &m_DisableSmoothing)) SaveSettings();

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
        ImGui::Text("Yaw: %.2f  Pitch: %.2f  Dist: %.1f", m_Yaw, m_Pitch, m_Distance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    ImGui::Unindent();
}