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
// Throw dispatch — the exact game function ACUFixes' "NoMoreFailedBombThrows"
// uses to force a bomb to be thrown.
// ---------------------------------------------------------------------------
DEFINE_GAME_FUNCTION(HumanStatesHolder__OnThrowBomb_P, 0x141988870, void, __fastcall,
    (HumanStatesHolder* a1, __m128* a2, char a3));

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
// Layer 1: BombAim camera forcing.
//
// The camera component holds `currentCameraSelectorBlenderNode`, a
// SharedPtrNew<CameraSelectorBlenderNode>* (i.e. a SharedBlock: manObj,
// refcounts, handle). We capture the block the first time the game itself is
// seen using the BombAimRegular handle, taking a +1 strong ref so the block
// can't be freed while cached. While pseudo-aiming we write that copy into the
// field every frame (the game's own camera selector may overwrite it — we win
// by re-applying). The game's next natural mode switch decrements the ref we
// took; worst case (if the game uses the field like a weak ref) is one bounded
// leaked strong ref per session — no crash.
// ---------------------------------------------------------------------------
static const char* GetCameraModeName(uint64_t handle)
{
    if (handle == kHandleBombAimRegular)  { return "BombAimRegular"; }
    if (handle == kHandleBombAimFromCover) { return "BombAimFromCover"; }
    return "other";
}

void BombSprintAimPlugin::TryForceBombAimCamera()
{
    ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
    if (!cam) { return; }

    __try
    {
        SharedPtrNew<CameraSelectorBlenderNode>* field = cam->currentCameraSelectorBlenderNode;
        if (!field) { return; }

        const uint64_t currentHandle = field->handle;
        m_CameraIsBombAim = (currentHandle == kHandleBombAimRegular);

        // Capture the BombAimRegular block the first time we observe it
        // (normally during a vanilla bomb aim). Keep observing even while not
        // pseudo-aiming so a cache eventually appears.
        if (!m_HaveCachedBombAimBlock && currentHandle == kHandleBombAimRegular)
        {
            field->IncrementStrongRefcount();
            m_CachedBombAimBlock = *field;
            m_HaveCachedBombAimBlock = true;
        }

        if (!m_PseudoAiming || !m_HaveCachedBombAimBlock) { return; }

        if (currentHandle != kHandleBombAimRegular)
        {
            *field = m_CachedBombAimBlock;
            ++m_CameraForcesApplied;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
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
// Core per-frame pseudo-aim logic.
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

        if (m_ForceCamera)          { TryForceBombAimCamera(); }
        if (m_DriveAimingProcess)   { DriveAimingProcess(); }
    }
    else
    {
        m_PseudoAiming = false;
    }

    // Release edge: dispatch the throw if we were pseudo-aiming while held.
    if (!aimHeld && m_WasAimHeld)
    {
        if (m_ThrowOnRelease && m_PseudoAiming)
        {
            const uint64_t heldMs = GetTickCount64() - m_AimHoldStartTick;
            if (heldMs >= (uint64_t)m_ThrowAfterHoldMs)
            {
                HumanStatesHolder* target = HumanStatesHolder::GetForPlayer();
                if (target)
                {
                    __try
                    {
                        __m128 zeroed;
                        zeroed.m128_f32[0] = 0.0f;
                        zeroed.m128_f32[1] = 0.0f;
                        zeroed.m128_f32[2] = 0.0f;
                        zeroed.m128_f32[3] = 0.0f;
                        HumanStatesHolder__OnThrowBomb_P(target, &zeroed, 1);
                        ++m_ThrowsDispatched;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                    }
                }
            }
        }
        m_PseudoAiming = false;
    }

    // Aim held without sprint: not pseudo-aiming anymore.
    if (aimHeld && !sprintHeld)
    {
        m_PseudoAiming = false;
    }

    // Remember the press edge BEFORE updating m_WasAimHeld.
    if (aimHeld && !m_WasAimHeld)
    {
        m_AimHoldStartTick = GetTickCount64();
    }

    m_WasAimHeld = aimHeld;
}

void BombSprintAimPlugin::OnUpdate()
{
    // The ImGui panel draws in the game's own update; keep it cheap.
    if (!m_Enabled) { return; }

    __try
    {
        // Observe the camera even when not pseudo-aiming (cache population).
        if (m_ForceCamera)
        {
            ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();
            if (cam && cam->currentCameraSelectorBlenderNode)
            {
                const uint64_t h = cam->currentCameraSelectorBlenderNode->handle;
                m_CameraIsBombAim = (h == kHandleBombAimRegular);
                if (!m_HaveCachedBombAimBlock && h == kHandleBombAimRegular)
                {
                    cam->currentCameraSelectorBlenderNode->IncrementStrongRefcount();
                    m_CachedBombAimBlock = *cam->currentCameraSelectorBlenderNode;
                    m_HaveCachedBombAimBlock = true;
                }
            }
        }

        UpdatePseudoAim();
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
    if (!ImGui::CollapsingHeader("Bomb Sprint Aim"))
    {
        return;
    }

    ImGui::Checkbox("Enable pseudo-aim", &m_Enabled);
    if (!m_Enabled) { return; }

    ImGui::Indent();

    ImGui::Checkbox("Force BombAim camera", &m_ForceCamera);
    ImGui::Checkbox("Drive ballistic aiming process", &m_DriveAimingProcess);
    ImGui::Checkbox("Throw bomb on release", &m_ThrowOnRelease);
    ImGui::SliderInt("Throw if held longer than (ms)", &m_ThrowAfterHoldMs, 100, 1000);

    ImGui::Separator();

    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    ACUPlayerCameraComponent* cam = ACU::GetPlayerCameraComponent();

    ImGui::Text("Pseudo-aiming: %s", m_PseudoAiming ? "YES" : "no");
    ImGui::Text("Sprint held: %s   Aim held: %s",
        IsSprintHeld() ? "yes" : "no",
        IsAimHeld() ? "yes" : "no");
    ImGui::Text("Frames pseudo-aiming: %d", m_FramesPseudoAiming);

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

    ImGui::Text("Cached BombAim camera block: %s",
        m_HaveCachedBombAimBlock ? "yes" : "no (aim a bomb normally once)");
    if (!m_HaveCachedBombAimBlock)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
            "Hint: hold BombDrop/RMB while standing still (vanilla aim) so the camera can be captured.");
    }

    if (hs)
    {
        ImGui::Text("Equipment: %d (%s)",
            (int)hs->ballisticAimingCurrentEquipmentType,
            GetEquipmentTypeName(hs->ballisticAimingCurrentEquipmentType));

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

    ImGui::Text("Throws dispatched: %d", m_ThrowsDispatched);
    ImGui::Text("Camera forces applied: %d", m_CameraForcesApplied);

    ImGui::Unindent();
}