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
#include <sstream>

static constexpr float PI = 3.14159265358979323846f;
static constexpr float VERTICAL_LIMIT = 1.48f;
static constexpr float MIN_FOV = 0.2f;
static constexpr float MAX_FOV = 2.0f;

PhotoModePlugin* g_pPhotoMode = nullptr;

// Small C-style SEH helpers: keep any __try blocks inside functions
// that have only POD locals so MSVC won't complain about C2712.
static ACUPlayerCameraComponent* SafeGetPlayerCameraComponent()
{
    ACUPlayerCameraComponent* cam = nullptr;
    __try { cam = ACU::GetPlayerCameraComponent(); } __except (EXCEPTION_EXECUTE_HANDLER) { cam = nullptr; }
    return cam;
}

static Entity* SafeGetPlayer()
{
    Entity* p = nullptr;
    __try { p = ACU::GetPlayer(); } __except (EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
    return p;
}

static void SafeApplyFreeCamera(PhotoModePlugin* photoMode, ACUPlayerCameraComponent* cam)
{
    __try { if (photoMode && cam) { switch (photoMode->GetMode()) { case PhotoModePlugin::Mode::Free: photoMode->ApplyFreeCamera(cam); break; default: break; } } } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

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

// Level-horizon orientation for the free camera: same look basis as above but
// keeps the horizon flat for ANY yaw (up stays closest to world up), with an
// optional static roll around the view axis (degrees). Used in normal freecam
// so the game's sprint/landing bob is overridden while mouse-yaw still turns
// horizontally with no tilt.
static Vector4f BuildLevelFreeCameraQuaternion(float yaw, float pitch, float rollDegrees)
{
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw), sy = sinf(yaw);

    Vector3f fwd(sy * cp, sp, cy * cp);
    Vector3f right(cy, 0.0f, -sy);
    Vector3f up(
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x);

    if (rollDegrees != 0.0f)
    {
        const float phi = rollDegrees * (PI / 180.0f);
        const float cr = cosf(phi), sr = sinf(phi);
        const Vector3f rolledRight = right * cr + up * sr;
        up = right * (-sr) + up * cr;
        right = rolledRight;
    }
    return QuaternionFromBasis(right, up, fwd);
}

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

                // hook hit accounting removed for ship-ready build

                auto* cam = (ACUPlayerCameraComponent*)params->r14_;
                if (!cam) return;

                SafeApplyFreeCamera(photoMode, cam);
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

    // ALWAYS override the orientation quaternion: the vanilla camera bakes
    // sprint/landing bob and shake into it, so leaving it alone lets freecam
    // bounce around (visible in Follow Player / Freeze Camera). Tilt Mode keeps
    // the old rolled look; normal freecam writes a level-horizon quaternion,
    // optionally rolled by m_TiltAngle (carry-over from Tilt Mode).
    if (m_TiltMode)
        cam->quaternion_mb = BuildFreeCameraQuaternion(m_Yaw, m_Pitch);
    else
        cam->quaternion_mb = BuildLevelFreeCameraQuaternion(m_Yaw, m_Pitch, m_TiltAngle);

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
        else if (line.rfind("FollowAllowMouse=", 0) == 0)
            m_FollowAllowMouse = (line.substr(17) == "1");
        else if (line.rfind("FollowIgnoreAllInput=", 0) == 0)
            m_FollowIgnoreAllInput = (line.substr(20) == "1");
        else if (line.rfind("FollowIgnoreExceptMouse=", 0) == 0)
            m_FollowIgnoreExceptMouse = (line.substr(24) == "1");
        else if (line.rfind("TiltMode=", 0) == 0)
            m_TiltMode = (line.substr(9) == "1");
        else if (line.rfind("TiltAngle=", 0) == 0)
            try { m_TiltAngle = std::stof(line.substr(10)); } catch (...) {}
        else if (line.rfind("MouseTilt=", 0) == 0)
            m_MouseTilt = (line.substr(10) == "1");
        else if (line.rfind("MouseTiltSensitivity=", 0) == 0)
            try { m_MouseTiltSensitivity = std::stof(line.substr(21)); } catch (...) {}
        else if (line.rfind("DisableYaw=", 0) == 0)
            m_DisableYaw = (line.substr(11) == "1");
        else if (line.rfind("DisablePitch=", 0) == 0)
            m_DisablePitch = (line.substr(13) == "1");
        else if (line.rfind("DisableRoll=", 0) == 0)
            m_DisableRoll = (line.substr(11) == "1");
        else if (line.rfind("BlockPositionMovement=", 0) == 0)
            ; // removed option: ignore for backward compatibility
        else if (line.rfind("FreezeCamera=", 0) == 0)
            m_FreezeCamera = (line.substr(13) == "1");
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
        
        // Load slot key bindings
        for (int i = 0; i < 9; ++i)
        {
            std::string prefix = "SlotKey" + std::to_string(i + 1) + "=";
            if (line.rfind(prefix, 0) == 0)
            {
                try { m_SlotKeys[i] = std::stoi(line.substr(prefix.length()), nullptr, 16); } 
                catch (...) {}
                break;
            }
        }
    }
    // If Tilt Mode was loaded as enabled, allow yaw control (user intent).
    if (m_TiltMode) m_DisableYaw = false;
}

// Recording/replay helper functions removed for ship-ready build.

static float LerpFloat(float a, float b, float t) { return a + (b - a) * t; }
static Vector3f LerpVec(const Vector3f& a, const Vector3f& b, float t) { return a * (1.0f - t) + b * t; }

// Interpolate yaw taking wraparound into account
static float InterpYaw(float a, float b, float t)
{
    float diff = b - a;
    while (diff > PI) diff -= 2*PI;
    while (diff < -PI) diff += 2*PI;
    return a + diff * t;
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
                 << "FollowAllowMouse=" << (m_FollowAllowMouse ? 1 : 0) << "\n"
                 << "FollowIgnoreAllInput=" << (m_FollowIgnoreAllInput ? 1 : 0) << "\n"
                 << "FollowIgnoreExceptMouse=" << (m_FollowIgnoreExceptMouse ? 1 : 0) << "\n"
                 << "FreezeCamera=" << (m_FreezeCamera ? 1 : 0) << "\n"
                 << "TiltMode=" << (m_TiltMode ? 1 : 0) << "\n"
                 << "TiltAngle=" << m_TiltAngle << "\n"
                 << "MouseTilt=" << (m_MouseTilt ? 1 : 0) << "\n"
                 << "MouseTiltSensitivity=" << m_MouseTiltSensitivity << "\n"
                 << "DisableYaw=" << (m_DisableYaw ? 1 : 0) << "\n"
                 << "DisablePitch=" << (m_DisablePitch ? 1 : 0) << "\n"
                 << "DisableRoll=" << (m_DisableRoll ? 1 : 0) << "\n"
                 << "MoveSpeed=" << m_MoveSpeed << "\n"
                 << "MouseSensitivity=" << m_MouseSensitivity << "\n"
                 << "InvertX=" << (m_InvertX ? 1 : 0) << "\n"
                 << "InvertY=" << (m_InvertY ? 1 : 0) << "\n"
                 << "DisableSmoothing=" << (m_DisableSmoothing ? 1 : 0) << "\n";
            
            // Save slot key bindings
            for (int i = 0; i < 9; ++i)
                file << "SlotKey" << (i + 1) << "=" << std::hex << m_SlotKeys[i] << std::dec << "\n";
            
            // Save camera slots
            for (int i = 0; i < 9; ++i)
            {
                if (!m_Slots[i].used) continue;
                std::string prefix = "Slot" + std::to_string(i + 1) + "_";
                file << prefix << "Used=1\n"
                     << prefix << "PosX=" << m_Slots[i].pos.x << "\n"
                     << prefix << "PosY=" << m_Slots[i].pos.y << "\n"
                     << prefix << "PosZ=" << m_Slots[i].pos.z << "\n"
                     << prefix << "Yaw=" << m_Slots[i].yaw << "\n"
                     << prefix << "Pitch=" << m_Slots[i].pitch << "\n"
                     << prefix << "Fov=" << m_Slots[i].fov << "\n"
                     << prefix << "Tilt=" << m_Slots[i].tilt << "\n";
            }
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

    // Freeze Camera: lock the pose in place — skip the mouse orbit and movement
    // below so Arno can be played without nudging the camera. Follow Player
    // behavior is now configurable via checkboxes: you can ignore all input,
    // ignore all except mouse, or allow inputs while following.
    const bool frozen = m_FreezeCamera && !m_FollowPlayer;
    const bool followIgnoreAll = m_FollowPlayer && m_FollowIgnoreAllInput;
    const bool followIgnoreExceptMouse = m_FollowPlayer && m_FollowIgnoreExceptMouse;
    // Determine whether mouse-based look is allowed this frame.
    bool mouseAllowed = !frozen;
    if (m_FollowPlayer)
    {
        if (followIgnoreAll) mouseAllowed = false;
        else if (followIgnoreExceptMouse) mouseAllowed = true;
        else mouseAllowed = m_FollowAllowMouse; // legacy behavior
    }
    const bool lookBlocked = !mouseAllowed;

    // Mouse orbit + FOV wheel (ignored while frozen, or while Following Player
    // unless "Allow Mouse Look" is on).
    if (!lookBlocked)
    {
        auto* inp = ACU::Input::Get_InputContainerBig();
        if (inp)
        {
            const int dx = inp->mouseState.mouseDeltaIntForCamera_X;
            const int dy = inp->mouseState.mouseDeltaIntForCamera_Y;
            if (dx != 0 || dy != 0)
            {
                const float xMult = m_InvertX ? 1.0f : -1.0f;
                const float yMult = m_InvertY ? -1.0f : 1.0f;
                if (m_MouseTilt)
                {
                    // Mouse Tilt: X dials the Tilt Angle (±180) so roll can be
                    // set by feel; Y still orbits pitch. Yaw may be disabled.
                    if (!m_DisableRoll)
                    {
                        m_TiltAngle += (float)dx * m_MouseSensitivity * m_MouseTiltSensitivity * xMult;
                        if (m_TiltAngle > 180.0f) m_TiltAngle = 180.0f;
                        if (m_TiltAngle < -180.0f) m_TiltAngle = -180.0f;
                    }
                    if (!m_DisablePitch)
                    {
                        m_Pitch += (float)dy * m_MouseSensitivity * yMult;
                        if (m_Pitch > VERTICAL_LIMIT) m_Pitch = VERTICAL_LIMIT;
                        if (m_Pitch < -VERTICAL_LIMIT) m_Pitch = -VERTICAL_LIMIT;
                    }
                }
                else
                {
                    if (!m_DisableYaw)
                    {
                        m_Yaw += (float)dx * m_MouseSensitivity * xMult;
                        m_Yaw = BringToIntervalWithWraparound(m_Yaw, -PI, PI);
                    }
                    if (!m_DisablePitch)
                    {
                        m_Pitch += (float)dy * m_MouseSensitivity * yMult;
                        if (m_Pitch > VERTICAL_LIMIT) m_Pitch = VERTICAL_LIMIT;
                        if (m_Pitch < -VERTICAL_LIMIT) m_Pitch = -VERTICAL_LIMIT;
                    }
                }
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

    // Arrow-key / QE fly. Movement is along the camera plane (flat forward/right)
    // and is independent of the game clock. Movement may be disabled while
    // following depending on the follow-player ignore settings.
    bool movementAllowed = !frozen && (!m_FollowPlayer || (!m_FollowIgnoreAllInput && !m_FollowIgnoreExceptMouse));
    if (movementAllowed)
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
        if (GetAsyncKeyState('E') & 0x8000) moveY += 1.0f;
        if (GetAsyncKeyState('Q') & 0x8000) moveY -= 1.0f;

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
    m_Slots[slot].tilt = m_TiltAngle;
    m_ActiveSlot = slot;
    SaveSettings(); // Persist the slot to INI file
    return slot;
}

void PhotoModePlugin::ApplyCameraSlot(int index)
{
    if (index < 0 || index >= 9 || !m_Slots[index].used) return;
    m_FreeCamPos = m_Slots[index].pos;
    m_Yaw = m_Slots[index].yaw;
    m_Pitch = m_Slots[index].pitch;
    m_Fov = m_Slots[index].fov;
    m_TiltAngle = m_Slots[index].tilt;
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

    // Direct slot selection via hotkeys (1-9)
    if (m_Mode != Mode::None)
    {
        for (int i = 0; i < 9; ++i)
        {
            const bool slotDown = (GetAsyncKeyState(m_SlotKeys[i]) & 0x8000) != 0;
            if (slotDown && !m_PrevSlotKeyDown[i] && m_Slots[i].used)
                ApplyCameraSlot(i);
            m_PrevSlotKeyDown[i] = slotDown;
        }
    }

    if (m_Mode == Mode::None) return;

    uint64 now = GetTickCount64();
    float dt = (float)(now - m_LastTick) / 1000.0f;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    m_LastTick = now;

    if (m_Mode == Mode::Free) UpdateFreeInput(dt);

    // Recording/replay removed for ship-ready build.
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
    ImGui::BulletText("Mouse: look / orbit");
    ImGui::BulletText("Mouse Tilt on: mouse X sets Tilt Angle (roll), Y orbits pitch");
    ImGui::BulletText("Mouse wheel: FOV zoom");
    ImGui::BulletText("Arrow keys: move camera (forward/back/left/right)");
    ImGui::BulletText("Q / E: move camera up / down");
    ImGui::BulletText("Shift: hold for 10x movement speed");
    ImGui::BulletText(", / .: switch between saved camera poses");
    ImGui::BulletText("Freeze Camera on: mouse & arrows ignored");
    ImGui::BulletText("Follow Player on: arrows, Q/E & mouse locked (Allow Mouse Look re-enables mouse)");

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
    // Hide Player UI removed per request (option still exists in state but is no longer exposed)
    if (ImGui::Checkbox("Follow Player", &m_FollowPlayer))
    {
        if (m_FollowPlayer)
        {
            m_FreezeWorld = false;
            // Anchor Follow to the CURRENT freecam pose so enabling it mid-shot
            // doesn't teleport the camera to player + stale offset.
            if (m_Mode == Mode::Free)
            {
                Entity* player = ACU::GetPlayer();
                if (player)
                    m_FollowOffset = m_FreeCamPos - player->GetPosition();
            }
        }
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
        ImGui::TextDisabled("Camera locked; move Arno without nudging the camera. Follow Player overrides this.");
        ImGui::Unindent();
    }
    if (ImGui::Checkbox("Tilt Mode", &m_TiltMode))
    {
        // Enabling Tilt Mode should re-enable yaw control for users who want it.
        if (m_TiltMode) m_DisableYaw = false;
        SaveSettings();
    }
    if (m_TiltMode)
    {
        ImGui::Indent();
        ImGui::TextDisabled("Tilt Mode: enables rolled look and allows yaw control.");
        ImGui::Unindent();
    }
    if (!m_TiltMode)
    {
        if (ImGui::SliderFloat("Tilt Angle", &m_TiltAngle, -180.0f, 180.0f, "%.1f deg")) SaveSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Static roll around the view axis in normal freecam — use Mouse Tilt to dial it by feel, or set it in Tilt Mode and carry the angle over.");
        if (ImGui::Checkbox("Mouse Tilt", &m_MouseTilt)) SaveSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mouse left/right adjusts the Tilt Angle instead of yaw (X = roll, Y = pitch). Yaw stays fixed while on.");
        if (m_MouseTilt)
        {
            ImGui::Indent();
            ImGui::TextDisabled("Mouse X now rolls the camera; use the slider or Y to orbit pitch.");
            if (ImGui::SliderFloat("Mouse Tilt Sensitivity", &m_MouseTiltSensitivity, 0.1f, 10.0f, "%.2f")) SaveSettings();
            ImGui::Unindent();
        }
    }
    if (m_FollowPlayer)
    {
        ImGui::Indent();
        if (ImGui::Checkbox("Ignore Camera Movement Input (Follow)", &m_FollowIgnoreAllInput))
        {
            if (m_FollowIgnoreAllInput) m_FollowIgnoreExceptMouse = false;
            SaveSettings();
        }
        if (ImGui::Checkbox("Ignore Camera Movement Input Except Mouse (Follow)", &m_FollowIgnoreExceptMouse))
        {
            if (m_FollowIgnoreExceptMouse) m_FollowIgnoreAllInput = false;
            SaveSettings();
        }
        // BlockPositionMovement option removed per UI simplification.
        ImGui::TextDisabled("Camera tracks Arno as he runs; world stays live.");
        ImGui::TextDisabled("Use the options above to block camera controls while following.");
        ImGui::Unindent();
    }
    if (ImGui::SliderFloat("Move Speed", &m_MoveSpeed, 0.5f, 20.0f, "%.1f")) SaveSettings();

    ImGui::Separator();

    if (ImGui::SliderFloat("FOV", &m_Fov, MIN_FOV, MAX_FOV, "%.2f")) SaveSettings();
    if (ImGui::Checkbox("Invert X", &m_InvertX)) SaveSettings();
    if (ImGui::Checkbox("Invert Y", &m_InvertY)) SaveSettings();
    // Per-axis disable UI removed; yaw is disabled by default and can be re-enabled by enabling Tilt Mode.
    if (ImGui::Checkbox("Disable Camera Smoothing", &m_DisableSmoothing)) SaveSettings();

    ImGui::Separator();

    ImGui::TextDisabled("SAVED CAMERAS");
    if (ImGui::Button("Save Current Pose"))
        SaveCurrentCameraSlot();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stores the current position/angle/FOV/tilt into a free slot (overwrites the active slot when full).");
    ImGui::SameLine();
    if (ImGui::Button("Clear All"))
    {
        for (int i = 0; i < 9; ++i) m_Slots[i].used = false;
        m_ActiveSlot = -1;
        SaveSettings(); // Persist the cleared slots
    }
    for (int i = 0; i < 9; ++i)
    {
        if (!m_Slots[i].used) continue;
        
        // Get the character representation of the slot key
        char keyChar = (char)m_SlotKeys[i];
        char label[64];
        if (keyChar >= 32 && keyChar < 127)
            sprintf_s(label, sizeof(label), "Slot %d [%c]%s", i + 1, keyChar, (m_ActiveSlot == i) ? " *" : "");
        else
            sprintf_s(label, sizeof(label), "Slot %d [0x%02X]%s", i + 1, m_SlotKeys[i], (m_ActiveSlot == i) ? " *" : "");
        
        if (ImGui::Button(label))
            ApplyCameraSlot(i);
        ImGui::SameLine();
        char clearLabel[32];
        sprintf_s(clearLabel, "Clear##%d", i);
        if (ImGui::Button(clearLabel))
        {
            m_Slots[i].used = false;
            if (m_ActiveSlot == i) m_ActiveSlot = -1;
            SaveSettings(); // Persist the cleared slot
        }
    }
    ImGui::TextDisabled("Use 1-9 keys, ',' or '.' to switch between saved poses");


    // Recording/replay UI removed for ship-ready build.

    ImGui::Separator();

    if (ImGui::Button("RESET CAMERA", ImVec2(-1.0f, 0.0f)))
        ResetCamera();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Restores the camera to where the game camera\n"
            "was when you entered the current mode.\n"
            "Hotkey: Reset Cam (F11).");

    ImGui::Separator();

    // Debug info removed for ship-ready build.

    ImGui::Unindent();
}