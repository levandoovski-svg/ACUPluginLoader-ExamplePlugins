#include "pch.h"
#include "BombSprintAimPlugin.h"

#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ACU/CameraSelectorBlenderNode.h"
#include "ACU/Enum_EquipmentType.h"
#include "Common_Plugins/ACU_InputUtils.h"

#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Camera mode handles (from ACUFixes Hack_ModifyAimingFOV)
//   Game Bootstrap Settings\TEMP BUGFIX Tools.CameraSelectorBlenderNode
//   Game Bootstrap Settings\Copy of Aiming 2 Cover.CameraSelectorBlenderNode
// ---------------------------------------------------------------------------
static constexpr uint64_t kHandleBombAimRegular  = 0x12F9251F30ull;
static constexpr uint64_t kHandleBombAimFromCover = 0x34CE205063ull;

// ---------------------------------------------------------------------------
// v1.5: manual throw dispatch REMOVED (v1 also forced OnThrowBomb_P,
// 0x141988870, on release). v2 will rely on the game's own AimBomb state to
// throw on release instead of forcing the call ourselves.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// State-name map for the debug readout (Enter addresses of known states).
// ---------------------------------------------------------------------------
struct StateNameEntry { uint64_t addr; const char* name; };
static const StateNameEntry kKnownStates[] = {
    { 0x141A6A900ull, "OnGroundHighProfile" },
    { 0x141A7AE90ull, "OnGroundLowProfile" },
    { 0x141A79930ull, "AimBomb" },
    { 0x141A798E0ull, "AimBomb_Deploying" },
    { 0x141A79580ull, "AimBomb_Deployed" },
    { 0x141A79800ull, "AimBomb_Stopping" },
    { 0x141AA4DD0ull, "AimGun_Aiming" },
    { 0x141AA6350ull, "AimGun_AimingP" },
    { 0x141AA68F0ull, "AimGun_Parent" },
};

static const char* GetStateName(uint64_t addr)
{
    for (const auto& e : kKnownStates)
    {
        if (e.addr == addr)
        {
            return e.name;
        }
    }
    return "?";
}

static const char* GetEquipmentTypeName(EquipmentType t)
{
    switch (t)
    {
    case EquipmentType::SmokeBomb:     return "SmokeBomb";
    case EquipmentType::MoneyPouch:    return "MoneyPouch";
    case EquipmentType::CherryBomb:    return "CherryBomb";
    case EquipmentType::PoisonBomb:    return "PoisonBomb";
    case EquipmentType::GuillotineGun: return "GuillotineGun";
    case EquipmentType::unk_0x19:      return "unk_0x19";
    default:                           return "other";
    }
}

// Replicated from ACUFixes Hack_LookbehindButton.cpp:
// the per-equipment ballistic aiming process inside HumanStatesHolder.
static BallisticProjectileAimingProcess& GetAimingProcessForCurrentEquipment(HumanStatesHolder& hs)
{
    switch (hs.ballisticAimingCurrentEquipmentType)
    {
    case EquipmentType::SmokeBomb:     return hs.aimingSmokeBomb;
    case EquipmentType::MoneyPouch:    return hs.aimingMoneyPouch;
    case EquipmentType::CherryBomb:    return hs.aimingCherryBomb;
    case EquipmentType::PoisonBomb:    return hs.aimingPoison;
    case EquipmentType::unk_0x19:      return hs.aiming_equip19_1770;
    case EquipmentType::GuillotineGun: return hs.aimingGuillotineGun;
    default:                           return hs.aimingDefault;
    }
}

// Is the bomb aim button currently held? (RMB is the generic aim button;
// holding BombDrop/F is the bomb-specific aim gesture per the HumanStatesHolder
// comments for the aim-start function at 0x14198F020.)
static bool IsAimHeld()
{
    return ACU::Input::IsPressedRMB() || ACU::Input::IsPressed(ActionKeyCode::BombDrop);
}

static bool IsSprintHeld()
{
    return ACU::Input::IsPressed(ActionKeyCode::Sprint);
}

// Scan the primary callback receivers for a human state node whose Enter
// function matches the given address (FirstPersonAimPlugin pattern).
static bool IsInState(uint64_t enterAddr)
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) { return false; }

    __try
    {
        for (auto& r : hs->primaryCallbackReceivers)
        {
            if (r.pNode && (uint64_t)r.pNode->Enter == enterAddr)
            {
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

// ---------------------------------------------------------------------------
// v1.5: camera forcing REMOVED (it caused the crash). We only READ the camera
// mode now — see OnUpdate().
// ---------------------------------------------------------------------------
static const char* GetCameraModeName(uint64_t handle)
{
    if (handle == kHandleBombAimRegular)  { return "BombAimRegular"; }
    if (handle == kHandleBombAimFromCover) { return "BombAimFromCover"; }
    return "other";
}

// ---------------------------------------------------------------------------
// Layer 2: keep the ballistic aiming process pointed along the camera.
// The AimBomb state normally updates these per frame; while pseudo-aiming we
// do it ourselves so the predictor/trajectory systems (if they run) see a
// sensible forward direction.
// ---------------------------------------------------------------------------
void BombSprintAimPlugin::DriveAimingProcess()
{
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
    if (!hs || !cam) { return; }

    __try
    {
        // Direction the camera is looking, in the ground plane.
        float dx = cam->locationLookat_A90.x - cam->locationSpinaround_AA0.x;
        float dy = cam->locationLookat_A90.y - cam->locationSpinaround_AA0.y;
        float dz = cam->locationLookat_A90.z - cam->locationSpinaround_AA0.z;

        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 0.0001f) { return; }

        dx /= len;
        dy /= len;
        dz /= len;

        BallisticProjectileAimingProcess& proc = GetAimingProcessForCurrentEquipment(*hs);

        Vector4f& fwd = proc.currentAimingState.vecForward_sorta_1b8;
        fwd.x = dx;
        fwd.y = dy;
        fwd.z = dz;
        fwd.w = 0.0f;

        Vector4f& flat = proc.vecForwardFlat_b0;
        flat.x = dx;
        flat.y = dy;
        flat.z = 0.0f;
        flat.w = 0.0f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ---------------------------------------------------------------------------
// Layer A (v2): bomb-aim FOV. While the pseudo-aim trigger is active, set the
// camera's live FOV fields (single floats) to the game's own bomb-aim FOV.
// Scalar write only — no selector graph, no refcounts — cannot reproduce the
// v1 crash. The game recomputes its own FOV whenever pseudo-aim is off, so no
// restore is needed.
// ---------------------------------------------------------------------------
void BombSprintAimPlugin::ApplyAimFov()
{
    if (!m_AimFovEnabled || !m_PseudoAiming) { return; }

    ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
    if (!cam) { return; }

    __try
    {
        cam->fov_mb_pi_4 = m_AimFovRadians;
        cam->fovPrecalc  = m_AimFovRadians;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ---------------------------------------------------------------------------
// Layer B (v2): OUR OWN ballistic arc overlay — the "custom raycast".
// 100% read-only (camera pose + calibrated throw constants), drawn as ImGui
// background lines. Draws when pseudo-aim is active OR the game is in any
// bomb-aim state — so the user calibrates throw speed / gravity against the
// game's own arc during a normal vanilla aim, and sprint-aim then shows the
// true trajectory.
// ---------------------------------------------------------------------------
static void CrossV3(const Vector4f& a, const Vector4f& b, Vector4f& out)
{
    out.x = a.y * b.z - a.z * b.y;
    out.y = a.z * b.x - a.x * b.z;
    out.z = a.x * b.y - a.y * b.x;
    out.w = 0.0f;
}

void BombSprintAimPlugin::DrawArcOverlay()
{
    if (!m_ArcOverlay) { return; }

    const bool inBombAim =
        IsInState(0x141A79930ull) ||  // AimBomb
        IsInState(0x141A798E0ull) ||  // AimBomb_Deploying
        IsInState(0x141A79580ull) ||  // AimBomb_Deployed
        IsInState(0x141A79800ull);    // AimBomb_Stopping
    if (!m_PseudoAiming && !inBombAim) { return; }

    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
    if (!hs || !cam) { return; }

    __try
    {
        ImGuiIO& io = ImGui::GetIO();
        const float W = io.DisplaySize.x;
        const float H = io.DisplaySize.y;
        if (W < 2.0f || H < 2.0f) { return; }
        const float aspect = W / H;

        // Camera basis (read-only): eye + forward from the camera pivots.
        Vector4f eye = cam->locationSpinaround_AA0;
        Vector4f fwd;
        fwd.x = cam->locationLookat_A90.x - eye.x;
        fwd.y = cam->locationLookat_A90.y - eye.y;
        fwd.z = cam->locationLookat_A90.z - eye.z;
        const float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
        if (fl < 0.0001f) { return; }
        fwd.x /= fl; fwd.y /= fl; fwd.z /= fl;

        Vector4f worldUp;
        worldUp.x = 0.0f;
        worldUp.y = m_WorldZUp ? 0.0f : 1.0f;
        worldUp.z = m_WorldZUp ? 1.0f : 0.0f;
        worldUp.w = 0.0f;

        Vector4f right, up;
        CrossV3(fwd, worldUp, right);
        const float rl = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
        if (rl < 0.0001f) { return; }
        right.x /= rl; right.y /= rl; right.z /= rl;
        CrossV3(right, fwd, up);

        // Projection: vertical FOV (use the FOV we apply, so the arc matches
        // the zoomed view).
        const float fovRad = m_PseudoAiming ? m_AimFovRadians : cam->fov_mb_pi_4;
        const float tanHalf = std::tan(fovRad * 0.5f);

        // Ballistic integration: p(t) = origin + v0*t + 0.5*g*t^2
        Vector4f vel;
        vel.x = fwd.x * m_ThrowSpeed;
        vel.y = fwd.y * m_ThrowSpeed;
        vel.z = fwd.z * m_ThrowSpeed;
        vel.w = 0.0f;
        Vector4f grav;
        grav.x = 0.0f;
        grav.y = m_WorldZUp ? 0.0f : -m_Gravity;
        grav.z = m_WorldZUp ? -m_Gravity : 0.0f;
        grav.w = 0.0f;

        Vector4f p = eye;
        const float dt = 1.0f / 30.0f;
        const int maxSteps = 180; // 6 seconds of flight

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const ImU32 arcColor  = IM_COL32(120, 220, 120, 235);
        const ImU32 landColor = IM_COL32(255, 140, 40, 255);

        const float groundY = m_WorldZUp ? eye.z : eye.y;
        bool haveLast = false;
        ImVec2 lastScreen(0.0f, 0.0f);

        for (int i = 0; i < maxSteps; ++i)
        {
            // project current point
            const float relX = p.x - eye.x;
            const float relY = p.y - eye.y;
            const float relZ = p.z - eye.z;
            const float zView = relX * fwd.x + relY * fwd.y + relZ * fwd.z;
            const float xView = relX * right.x + relY * right.y + relZ * right.z;
            const float yView = relX * up.x + relY * up.y + relZ * up.z;

            if (zView > 0.05f)
            {
                const float sx = ((xView / (zView * tanHalf * aspect)) + 1.0f) * 0.5f * W;
                const float sy = (1.0f - (yView / (zView * tanHalf))) * 0.5f * H;
                const ImVec2 curr(sx, sy);
                if (haveLast)
                {
                    dl->AddLine(lastScreen, curr, arcColor, 2.0f);
                }
                lastScreen = curr;
                haveLast = true;
            }
            else
            {
                haveLast = false; // behind the camera
            }

            // integrate one step
            vel.x += grav.x * dt; vel.y += grav.y * dt; vel.z += grav.z * dt;
            const float prevY = m_WorldZUp ? p.z : p.y;
            p.x += vel.x * dt; p.y += vel.y * dt; p.z += vel.z * dt;
            const float newY = m_WorldZUp ? p.z : p.y;

            // ground-plane landing: first crossing of the camera height
            if (prevY >= groundY && newY < groundY)
            {
                const float tDelta = (groundY - prevY) / (newY - prevY + 0.0001f);
                const float hitX = p.x - vel.x * dt * (1.0f - tDelta);
                const float hitY = p.y - vel.y * dt * (1.0f - tDelta);
                const float hitZ = p.z - vel.z * dt * (1.0f - tDelta);
                const float hRelX = hitX - eye.x;
                const float hRelY = hitY - eye.y;
                const float hRelZ = hitZ - eye.z;
                const float hz = hRelX * fwd.x + hRelY * fwd.y + hRelZ * fwd.z;
                if (hz > 0.05f)
                {
                    const float hx = hRelX * right.x + hRelY * right.y + hRelZ * right.z;
                    const float hy = hRelX * up.x + hRelY * up.y + hRelZ * up.z;
                    const float hsx = ((hx / (hz * tanHalf * aspect)) + 1.0f) * 0.5f * W;
                    const float hsy = (1.0f - (hy / (hz * tanHalf))) * 0.5f * H;
                    dl->AddCircleFilled(ImVec2(hsx, hsy), 5.0f, landColor, 12);
                }
                break;
            }
            if (newY < groundY - 20.0f) { break; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ---------------------------------------------------------------------------
// Core per-frame pseudo-aim logic (observe-only in v1.5).
// ---------------------------------------------------------------------------
void BombSprintAimPlugin::UpdatePseudoAim()
{
    if (!m_Enabled) { return; }

    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) { return; }

    const bool aimHeld = IsAimHeld();
    const bool sprintHeld = IsSprintHeld();
    const bool wantPseudoAim = m_Enabled && aimHeld && sprintHeld;

    if (wantPseudoAim)
    {
        m_PseudoAiming = true;
        ++m_FramesPseudoAiming;

        // DIAGNOSTIC ONLY (default OFF): writes only the ballistic process
        // forward vector. Was not proven to be the crash cause, but leave off.
        if (m_DriveAimingProcess) { DriveAimingProcess(); }
    }
    else
    {
        m_PseudoAiming = false;
    }
}

void BombSprintAimPlugin::OnUpdate()
{
    if (!m_Enabled) { return; }

    __try
    {
        // READ-ONLY camera observation — never writes the selector state.
        ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
        if (cam && cam->currentCameraSelectorBlenderNode)
        {
            const uint64_t h = cam->currentCameraSelectorBlenderNode->handle;
            m_CameraIsBombAim = (h == kHandleBombAimRegular);
        }

        UpdatePseudoAim();
        ApplyAimFov();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void BombSprintAimPlugin::OnBeforeActivate()
{
    // v1 has no code patches; nothing to install.
}

void BombSprintAimPlugin::OnBeforeDeactivate()
{
}

// ---------------------------------------------------------------------------
// ImGui debug panel
// ---------------------------------------------------------------------------
void BombSprintAimPlugin::OnImGuiRender()
{
    // v2: apply the FOV write + draw the arc overlay independent of whether
    // the debug panel is expanded. ApplyAimFov() here runs after the game's
    // own camera update, so the zoom sticks through the frame's render.
    ApplyAimFov();
    DrawArcOverlay();

    if (!ImGui::CollapsingHeader("Bomb Sprint Aim"))
    {
        return;
    }

    ImGui::Checkbox("Enable", &m_Enabled);
    if (!m_Enabled) { return; }

    ImGui::Indent();

    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.6f, 1.0f),
        "v2: bomb-aim FOV + custom arc overlay — no aim-state forcing.");
    ImGui::Checkbox("Drive ballistic aiming process (DIAGNOSTIC, default off)", &m_DriveAimingProcess);

    ImGui::Separator();
    ImGui::Checkbox("Bomb-aim FOV while sprint-aiming", &m_AimFovEnabled);
    ImGui::SliderFloat("Aim FOV (radians)", &m_AimFovRadians, 0.35f, 0.90f, "%.3f");
    ImGui::Checkbox("Custom arc overlay (read-only raycast)", &m_ArcOverlay);
    ImGui::Checkbox("World is Z-up", &m_WorldZUp);
    ImGui::SliderFloat("Throw speed (m/s)", &m_ThrowSpeed, 2.0f, 20.0f, "%.1f");
    ImGui::SliderFloat("Gravity (m/s^2)", &m_Gravity, 1.0f, 25.0f, "%.1f");
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f),
        "Calibrate: vanilla-aim a bomb; tune speed/gravity until the green arc\nmatches the game arc; sprint-aim then reuses the same constants.");

    ImGui::Separator();

    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();

    ImGui::Text("Pseudo-aim trigger (sprint+aim): %s", m_PseudoAiming ? "ACTIVE" : "inactive");
    ImGui::Text("Sprint held: %s   Aim held: %s",
        IsSprintHeld() ? "yes" : "no",
        IsAimHeld() ? "yes" : "no");
    ImGui::Text("Frames with trigger active: %d", m_FramesPseudoAiming);

    if (cam && cam->currentCameraSelectorBlenderNode)
    {
        const uint64_t h = cam->currentCameraSelectorBlenderNode->handle;
        ImGui::Text("Camera mode: 0x%llX (%s)%s",
            h, GetCameraModeName(h),
            m_CameraIsBombAim ? "  <== BombAim" : "");
    }
    else
    {
        ImGui::Text("Camera mode: (null)");
    }

    if (hs)
    {
        ImGui::Text("Equipment: %d (%s)",
            (int)hs->ballisticAimingCurrentEquipmentType,
            GetEquipmentTypeName(hs->ballisticAimingCurrentEquipmentType));

        const Timer& disallow = hs->timer_disallowSprintAndAimBombAfterLastAimBomb;
        ImGui::Text("Sprint<->AimBomb disallow timer: %s (end tick %llu)",
            disallow.isActive_mb_20 ? "ACTIVE" : "idle",
            disallow.timestampEnd);

        int n = 0;
        __try
        {
            for (auto& r : hs->primaryCallbackReceivers)
            {
                if (n >= 8) { break; }
                if (!r.pNode) { continue; }
                const uint64_t addr = (uint64_t)r.pNode->Enter;
                ImGui::Text("  state: 0x%llX %s", addr, GetStateName(addr));
                ++n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    ImGui::Unindent();
}