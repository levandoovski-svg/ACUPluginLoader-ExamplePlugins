#include "pch.h"
#include "ExactPositionTeleportPlugin.h"

#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include "ACU/Human.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/InputContainer.h"
#include "ACU/SmallArray.h"
#include "ACU_DefineNativeFunction.h"
#include "Common_Plugins/ACU_InputUtils.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ParkourDebugging/EnumParkourAction.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <shlobj.h>
#include <xmmintrin.h>

namespace
{
    constexpr int kDefaultForceKey = 'T'; // VK_T
    constexpr int kDefaultBreakfallDelayMs = 100;

    // ACUFixes-0.9.3 addresses for the build represented by the runtime dump
    // in MODLOG/breakfall-gate.md (base 0x140000000, no ASLR).
    //
    // BaseEntity::SetTransform(Entity* this, const Matrix4f* newTransform)
    // copies the supplied matrix into the entity's main transform. The
    // ACUFixes hook confirms the RCX/RDX calling convention.
    DEFINE_GAME_FUNCTION(BaseEntity__SetTransform, 0x1425DE360, void, __fastcall,
        (Entity* entity, const Matrix4f* newTransform));

    // This is the reset call used by ACUFixes-0.9.3's debug TeleportPlayer
    // helper after it writes the player's position.
    DEFINE_GAME_FUNCTION(Human__Dispatch_HSTE_ResetBehavior, 0x14131A530, char, __fastcall,
        (Human* human));

    // Parkour action REQUEST pipeline. The entry thunk clears the flag
    // register and jumps to the real request implementation. Calling with
    // EnumParkourAction::breakfall (18) asks the game to execute the normal
    // breakfall action rather than redirecting a state Enter directly.
    DEFINE_GAME_FUNCTION(ParkourAction__Request, 0x140C85280, void, __fastcall,
        (int actionId, int actionFlags));

    // ---------------------------------------------------------------------
    // Native raycast API (ACUFixes-0.9.3 addresses, byte-verified against the
    // runtime dump; see MODLOG/scripts/scanner_raycast_out.txt).
    //
    // `World` is forward-declared by ACU/ACUGetSingletons.h and must remain the
    // global type, so it is intentionally not re-declared in this namespace.
    // ---------------------------------------------------------------------

    // RaycastResult, size 0x60 (stride used by the game's sort routine).
    class PluginRaycastResult
    {
    public:
        Vector4f hitLocation;      //0x0000
        Vector4f targetNormal;     //0x0010
        void* physicCpnt;          //0x0020
        void* shared_targetEntity; //0x0028
        uint32 entityId_mb;        //0x0030
        char pad_0034[4];
        float distance_mb;         //0x0038
        char pad_003C[0x60 - 0x3C];
    };
    static_assert(sizeof(PluginRaycastResult) == 0x60, "RaycastResult must be 0x60");

    // RaycastStruct, size 0x680. The native ctor/raycast can use aligned SSE
    // stores on the struct and its inline result buffer (at +0x20), so force
    // 16-byte alignment.
    struct alignas(16) PluginRaycastStruct
    {
        char pad_0[8];
        SmallArray<PluginRaycastResult> raycastResults; //0x0008
        char pad_14[0x620 - 0x14];
        SmallArray<void*> excludedEntities_mb;          //0x0620
        char pad_62C[0x650 - 0x62C];
        uint64 raycastFlags;                            //0x0650
        char pad_658[0x680 - 0x658];
    };
    static_assert(sizeof(PluginRaycastStruct) == 0x680, "RaycastStruct must be 0x680");

    DEFINE_GAME_FUNCTION(RaycastStruct__ctor, 0x1425EC5A0, void*, __fastcall,
        (void* this_));
    DEFINE_GAME_FUNCTION(RaycastStruct__CastRay, 0x1425FD7D0, char, __fastcall,
        (void* this_, World* world, __m128* p_origin, __m128* p_direction,
            float p_rayLength, unsigned __int8 p_1forGunshot, char p_0forGunshot));
    DEFINE_GAME_FUNCTION(ArrRaycastResults__Destroy, 0x140010340, void, __fastcall,
        (void* arr));

    struct PluginRaycastHit
    {
        Vector3f position { 0.0f, 0.0f, 0.0f };
        Vector3f normal { 0.0f, 0.0f, 0.0f };
        float distance = 0.0f;
    };

    struct LedgeProbeOutcome
    {
        bool found = false;
        int raysCast = 0;
        int wallsFound = 0;
        float bestDistance = 0.0f;
        float bestLedgeRise = 0.0f;
        Vector3f bestDirection { 0.0f, 0.0f, 0.0f };
    };

    bool BytesMatch(uintptr_t address, const unsigned char* expected, size_t count)
    {
        __try
        {
            return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    Entity* GetPlayerSafely()
    {
        __try
        {
            return ACU::GetPlayer();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    Human* GetPlayerHumanSafely()
    {
        __try
        {
            return Human::GetForPlayer();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    World* GetWorldSafely()
    {
        __try
        {
            return ACU::GetWorld();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ApplyNativeSetTransform(Entity* entity, const Matrix4f* transform)
    {
        __try
        {
            BaseEntity__SetTransform(entity, transform);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool DispatchHumanReset(Human* human)
    {
        __try
        {
            Human__Dispatch_HSTE_ResetBehavior(human);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool RequestParkourActionSafely(int actionId)
    {
        __try
        {
            ParkourAction__Request(actionId, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The native calls below are kept in small SEH wrappers that only take
    // scalars/pointers. This keeps the __try blocks out of functions that
    // would otherwise require C++ object unwinding.
    bool RaycastCtorSafely(void* raycastStruct)
    {
        __try
        {
            RaycastStruct__ctor(raycastStruct);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool RaycastCastSafely(void* raycastStruct, World* world, __m128* origin, __m128* direction, float range)
    {
        __try
        {
            RaycastStruct__CastRay(raycastStruct, world, origin, direction, range, 1, 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void RaycastDestroySafely(void* resultsArray)
    {
        __try
        {
            ArrRaycastResults__Destroy(resultsArray);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool IsShiftHeld()
    {
        return (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0
            || (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    }

    // Treat Shift as released for the game while the player is mid-jump
    // (InSafeJumpOrLoweringToHang). Sprint/locomotion inputs fighting the
    // forced-freefall transition is what caused the "walking in air" jitter.
    //
    // This writes directly into the game's live pressed-state arrays - the
    // same ones ACU::Input::IsPressed() reads in ACUFixes-0.9.3
    // (Common_PluginSide ACU_InputUtils.cpp). Both the scancode-level
    // (K_LSHIFT = 42 / K_RSHIFT = 54) and the action-level (Sprint = 0xC)
    // entries are cleared so neither the raw nor the derived input path
    // sees Shift held. Re-applied every frame while the condition holds;
    // stopped as soon as the state ends or Shift is physically released.
    bool NegateShiftForGame()
    {
        __try
        {
            InputContainerBig* inputContainer = ACU::Input::Get_InputContainerBig();
            if (!inputContainer)
            {
                return false;
            }
            inputContainer->isPressed_byScancode[(uint32)BindableKeyCode_Keyboard::K_LSHIFT] = 0;
            inputContainer->isPressed_byScancode[(uint32)BindableKeyCode_Keyboard::K_RSHIFT] = 0;
            inputContainer->keyStates_thisFrame.isPressed_byAction[(int)ActionKeyCode::Sprint] = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Single raycast; returns the nearest hit (by distance) from the native
    // result array. `origin`/`direction` are the four-float vectors the native
    // wrapper expects (they are loaded with MOVAPS, so they must be 16-byte
    // aligned -- __m128 guarantees that).
    bool CastRayNearest(World* world, const Vector3f& origin, const Vector3f& direction,
        float range, PluginRaycastHit& outHit)
    {
        if (!world)
        {
            return false;
        }

        PluginRaycastStruct raycast;
        if (!RaycastCtorSafely(&raycast))
        {
            return false;
        }

        // Mirrors ACUFixes-0.9.3 Raycasting::RaycastStruct_CastRay.
        raycast.raycastFlags = ~static_cast<uint64>(0);

        __m128 originAlign = _mm_setr_ps(origin.x, origin.y, origin.z, 0.0f);
        __m128 directionAlign = _mm_setr_ps(direction.x, direction.y, direction.z, 0.0f);

        bool found = false;
        if (RaycastCastSafely(&raycast, world, &originAlign, &directionAlign, range))
        {
            SmallArray<PluginRaycastResult>& results = raycast.raycastResults;
            if (results.arr && results.size > 0)
            {
                float bestDistance = 3.4e38f;
                for (int i = 0; i < results.size; ++i)
                {
                    const PluginRaycastResult& hit = results.arr[i];
                    if (hit.distance_mb < bestDistance)
                    {
                        bestDistance = hit.distance_mb;
                        outHit.position = Vector3f(hit.hitLocation.x, hit.hitLocation.y, hit.hitLocation.z);
                        outHit.normal = Vector3f(hit.targetNormal.x, hit.targetNormal.y, hit.targetNormal.z);
                        outHit.distance = hit.distance_mb;
                        found = true;
                    }
                }
            }
        }

        if (raycast.raycastResults.arr)
        {
            RaycastDestroySafely(&raycast.raycastResults);
        }
        return found;
    }

    // The raycast "ledge gate".
    //
    // For a ring of horizontal directions around the player:
    //   1. cast a horizontal ray at (feet + WallProbeHeight) and accept it as
    //      a wall only if the surface normal is close to horizontal,
    //   2. step a little past that wall face and cast straight down within
    //      [feet + MinLedgeRise, feet + MaxLedgeRise], accepting the surface
    //      only if it is close to horizontal (a walkable/grabbable top).
    //
    // This mirrors the native freefall ledge scan at 0x141A3C050, which is fed
    // the player's feet position (ACUFixes shifts it 1.0 m toward the wall to
    // widen the search), but without depending on the Denuvo-virtualized
    // decision itself.
    LedgeProbeOutcome ProbeForLedge(World* world, const Vector3f& feet,
        int directions, float maxHorizontalReach, float wallProbeHeight,
        float wallNormalMaxAbsZ, float ledgeInset, float minLedgeRise,
        float maxLedgeRise, float topNormalMinZ)
    {
        LedgeProbeOutcome outcome;
        if (!world || directions <= 0 || maxHorizontalReach <= 0.0f)
        {
            return outcome;
        }

        const float twoPi = 6.28318530718f;

        // Sample two heights so both chest-high walls and waist-high walls
        // (the case the tester reported) are caught.
        const float probeHeights[2] = { wallProbeHeight, wallProbeHeight * 0.4f };

        for (int i = 0; i < directions; ++i)
        {
            const float angle = twoPi * static_cast<float>(i) / static_cast<float>(directions);
            const Vector3f dir(std::cos(angle), std::sin(angle), 0.0f);

            for (const float probeHeight : probeHeights)
            {
                PluginRaycastHit wallHit;
                const Vector3f wallOrigin(feet.x, feet.y, feet.z + probeHeight);
                if (!CastRayNearest(world, wallOrigin, dir, maxHorizontalReach, wallHit))
                {
                    continue;
                }
                ++outcome.raysCast;

                if (std::fabs(wallHit.normal.z) > wallNormalMaxAbsZ)
                {
                    // Ground/ceiling rather than a vertical wall face.
                    continue;
                }
                ++outcome.wallsFound;

                // Probe for the top of that wall just past its face.
                const Vector3f topOrigin(
                    wallHit.position.x + dir.x * ledgeInset,
                    wallHit.position.y + dir.y * ledgeInset,
                    feet.z + maxLedgeRise + 0.25f);
                const float topRange = (maxLedgeRise - minLedgeRise) + 0.5f;
                const Vector3f down(0.0f, 0.0f, -1.0f);

                PluginRaycastHit topHit;
                if (!CastRayNearest(world, topOrigin, down, topRange, topHit))
                {
                    continue;
                }
                ++outcome.raysCast;

                if (topHit.normal.z < topNormalMinZ)
                {
                    continue;
                }

                const float ledgeRise = topHit.position.z - feet.z;
                if (ledgeRise < minLedgeRise || ledgeRise > maxLedgeRise)
                {
                    continue;
                }

                if (!outcome.found || wallHit.distance < outcome.bestDistance)
                {
                    outcome.found = true;
                    outcome.bestDistance = wallHit.distance;
                    outcome.bestLedgeRise = ledgeRise;
                    outcome.bestDirection = dir;
                }
            }
        }

        return outcome;
    }

    // ---------------------------------------------------------------------
    // Human-state gate.
    //
    // The player's "HumanStates" are a tree of FunctorBase nodes; the type of
    // each node is identified by its Enter() function pointer. These Enter
    // addresses come from ACUFixes-0.9.3 ParkourDebugging/LoggingTheHumanStates.cpp
    // and were verified against the runtime dump (all six are valid function
    // starts in .pdata for this build).
    // ---------------------------------------------------------------------
    constexpr uintptr_t kStateOnGroundLowProfile = 0x141A7AE90;
    constexpr uintptr_t kStateOnGroundHighProfile = 0x141A6A900; // == OnGroundOverlookingEdgeHighProfile
    constexpr uintptr_t kStateOnPeakOrLedge = 0x141A60B80;
    constexpr uintptr_t kStateWalling = 0x141A4EC10;
    constexpr uintptr_t kStateInSafeJumpOrLoweringToHang = 0x141ADD2D0;
    constexpr uintptr_t kStateWallrunUp = 0x141A8CF40;
    constexpr uintptr_t kStateDetachingFromWall = 0x141A4BC20;

    struct ActiveHumanStates
    {
        bool onGroundLowProfile = false;
        bool onGroundHighProfile = false;
        bool onPeakOrLedge = false;
        bool walling = false;
        bool inSafeJumpOrLoweringToHang = false;
        bool wallrunUp = false;
        bool detachingFromWall = false;
        int nodeCount = 0;
    };

    void CollectActiveStatesWorker(const FunctorBase* node, ActiveHumanStates& out, int depth, int& budget)
    {
        if (!node || depth > 48 || budget <= 0)
        {
            return;
        }
        --budget;
        ++out.nodeCount;

        switch (reinterpret_cast<uintptr_t>(node->Enter))
        {
        case kStateOnGroundLowProfile:             out.onGroundLowProfile = true; break;
        case kStateOnGroundHighProfile:            out.onGroundHighProfile = true; break;
        case kStateOnPeakOrLedge:                  out.onPeakOrLedge = true; break;
        case kStateWalling:                        out.walling = true; break;
        case kStateInSafeJumpOrLoweringToHang:     out.inSafeJumpOrLoweringToHang = true; break;
        case kStateWallrunUp:                      out.wallrunUp = true; break;
        case kStateDetachingFromWall:              out.detachingFromWall = true; break;
        default: break;
        }

        CollectActiveStatesWorker(node->directChild_mb, out, depth + 1, budget);
        for (int i = 0; i < node->nonoverridingChildren.size; ++i)
        {
            CollectActiveStatesWorker(node->nonoverridingChildren.arr[i], out, depth + 1, budget);
        }
        for (int i = 0; i < node->someNodes_B8.size; ++i)
        {
            CollectActiveStatesWorker(node->someNodes_B8.arr[i], out, depth + 1, budget);
        }
    }

    bool CollectActiveStates(ActiveHumanStates& out)
    {
        __try
        {
            HumanStatesHolder* root = HumanStatesHolder::GetForPlayer();
            if (!root)
            {
                return false;
            }
            int budget = 1024;
            CollectActiveStatesWorker(reinterpret_cast<const FunctorBase*>(root), out, 0, budget);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::string GetDocumentsIniPath()
    {
        char documents[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, documents)))
        {
            return std::string(documents) + "\\Assassin's Creed Unity\\ExactPositionTeleport.ini";
        }
        return "ExactPositionTeleport.ini";
    }

    bool WriteDefaultIni(const std::filesystem::path& path)
    {
        std::ofstream file(path, std::ios::trunc);
        if (!file)
        {
            return false;
        }

        file << "// ExactPositionTeleport settings\n"
             << "// ForceKey is a Windows virtual-key code in hexadecimal (54 = T).\n"
             << "ForceKey=" << std::hex << kDefaultForceKey << std::dec << "\n"
             << "// Dispatch the HSTE ResetBehavior after the transform write.\n"
             << "// This is what forces the parkour system to re-evaluate the context\n"
             << "// and drop into freefall; turn off to A/B test the crash.\n"
             << "ResetBehaviorAfterTeleport=1\n"
             << "// Request a parkour action (default 18 = EnumParkourAction::breakfall)\n"
             << "// after the exact-position teleport.\n"
             << "// OFF BY DEFAULT: the breakfall-request layer is the identified crash\n"
             << "// source. VALIDATED STABLE CONFIG is reset ON + breakfall OFF (30+ min\n"
             << "// of constant use, zero crashes); breakfall natively with E in freefall.\n"
             << "BreakfallAfterTeleport=0\n"
             << "// While the player is mid-jump (InSafeJumpOrLoweringToHang), make the\n"
             << "// game see Shift as released so sprint cannot fight the forced\n"
             << "// freefall (prevents jittering / walking-in-air).\n"
             << "NegateShiftInSafeJump=1\n"
             << "// Keep re-issuing the breakfall request for as long as ForceKey is held.\n"
             << "HoldBreakfallWhileHeld=1\n"
             << "BreakfallActionId=18\n"
             << "// Delay in milliseconds between the teleport/reset and the first breakfall request.\n"
             << "BreakfallDelayMs=" << kDefaultBreakfallDelayMs << "\n"
             << "// While held: 0 = re-issue the request every frame.\n"
             << "BreakfallHoldIntervalMs=0\n"
             << "// Human-state gate: block while the player is in OnGroundLowProfile,\n"
             << "// OnGroundHighProfile, OnPeakOrLedge or Walling.\n"
             << "StateGateEnabled=1\n"
             << "// Also block unless the player is in InSafeJumpOrLoweringToHang or WallrunUp.\n"
             << "StateGateRequireValid=0\n"
             << "// Minimum milliseconds between two force-freefall/teleport attempts.\n"
             << "ForceCooldownMs=300\n"
             << "// Raycast ledge gate: only force-freefall when a wall with a usable\n"
             << "// top ledge is detected around the player.\n"
             << "RequireLedge=1\n"
             << "ProbeDirections=12\n"
             << "MaxHorizontalReach=1.5\n"
             << "WallProbeHeight=0.9\n"
             << "WallNormalMaxAbsZ=0.35\n"
             << "LedgeInset=0.35\n"
             << "MinLedgeRise=-1.0\n"
             << "MaxLedgeRise=2.5\n"
              << "TopNormalMinZ=0.6\n"
              << "// Safe injection: queues the HSTE reset and executes it inside a\n"
              << "// per-frame game-thread hook (0x141F3FE3B) instead of the Present\n"
              << "// thread; also disables the external breakfall-request layer.\n"
              << "SafeInjectionOnly=0\n"
              << "// Buffer (ms) after first valid-state detection before teleport.\n"
              << "StateDetectionBufferMs=200\n"
              << "// Re-apply teleport/freefall mechanism continuously while held.\n"
              << "ReapplyTeleportWhileHeld=0\n"
              << "// Master toggle: when 0, the plugin never touches the game.\n"
              << "PluginEnabled=1\n"
              << "// Show the collapsible debug/parameters section in the UI.\n"
              << "ShowDebugUi=0\n"
              << "// 0 = skip the transform write, dispatch only the HSTE reset.\n"
              << "TeleportBeforeReset=1\n";
        file.flush();
        return static_cast<bool>(file);
    }
}

// ---------------------------------------------------------------------------
// Deferred game-thread hook ("safe injection method").
//
// The plugin's OnUpdate() runs on the Present/render thread. Calling
// Human__Dispatch_HSTE_ResetBehavior from there mutates state objects the
// render frame does not expect to change (matches crash stack 0x1400EEC05,
// near the atom-graph/state container region).
//
// When SafeInjectionOnly is enabled, the reset is instead QUEUED during the
// teleport and executed inside this per-frame game-thread hook at
// 0x141F3FE3B (the FOV-per-frame site, 6 stolen bytes, already proven safe
// by FreeCameraRotationPlugin / PhotoModePlugin / photosavemod). The game's
// own state machine therefore performs the reset at a point it expects
// mutations - the same timing ACUFixes' TeleportPlayer gets from its own
// hook contexts.
// ---------------------------------------------------------------------------
ExactPositionTeleportPlugin* g_ExactTeleportDeferred = nullptr;

struct DeferredResetHook : AutoAssemblerCodeHolder_Base
{
    DeferredResetHook()
    {
        constexpr uintptr_t whenSettingFOVforFrame = 0x141F3FE3B;
        PresetScript_CCodeInTheMiddle(whenSettingFOVforFrame, 6,
            [](AllRegisters* params)
            {
                if (g_ExactTeleportDeferred)
                {
                    g_ExactTeleportDeferred->ServiceDeferredReset();
                }
            });
    }
};
static std::unique_ptr<AutoAssembleWrapper<DeferredResetHook>> g_DeferredResetHook;

void ExactPositionTeleportPlugin::Init()
{
    // Prefer the directory containing ACU.exe. If that directory is not
    // writable (for example, a protected Program Files install), fall back to
    // the standard writable Assassin's Creed Unity documents directory.
    m_IniPath = ResolvePrimaryIniPath();
    const bool iniReady = EnsureIniFile();
    LoadSettings();

    // Fail closed if this DLL is loaded into another ACU build. These are
    // exact signatures observed in the user's runtime dump, not guessed.
    static const unsigned char expectedSetTransform[] = {
        0x0F, 0x28, 0x02, 0x0F, 0x29, 0x41, 0x20, 0x0F
    };
    static const unsigned char expectedResetBehavior[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20
    };
    static const unsigned char expectedParkourRequest[] = {
        0x33, 0xD2, 0xE9, 0x09, 0x00, 0x00, 0x00
    };
    static const unsigned char expectedRaycastCtor[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
        0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xF9
    };
    static const unsigned char expectedRaycastCast[] = {
        0x48, 0x83, 0xEC, 0x48, 0x0F, 0xB6, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00
    };
    static const unsigned char expectedRaycastDestroy[] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x41, 0x08
    };

    const bool setTransformMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(BaseEntity__SetTransform),
        expectedSetTransform,
        sizeof(expectedSetTransform));
    const bool resetBehaviorMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(Human__Dispatch_HSTE_ResetBehavior),
        expectedResetBehavior,
        sizeof(expectedResetBehavior));
    const bool parkourRequestMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(ParkourAction__Request),
        expectedParkourRequest,
        sizeof(expectedParkourRequest));
    const bool raycastCtorMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(RaycastStruct__ctor),
        expectedRaycastCtor,
        sizeof(expectedRaycastCtor));
    const bool raycastCastMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(RaycastStruct__CastRay),
        expectedRaycastCast,
        sizeof(expectedRaycastCast));
    const bool raycastDestroyMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(ArrRaycastResults__Destroy),
        expectedRaycastDestroy,
        sizeof(expectedRaycastDestroy));

    m_BuildCompatible = setTransformMatches && resetBehaviorMatches;
    m_BreakfallRequestCompatible = parkourRequestMatches;
    m_RaycastCompatible = raycastCtorMatches && raycastCastMatches && raycastDestroyMatches;
    m_PrevKeyDown = (GetAsyncKeyState(m_ForceKey) & 0x8000) != 0;

    Log("ini=" + m_IniPath + " log=" + GetLogPath());

    if (!iniReady)
    {
        SetStatus("WARNING: could not create the settings INI file");
    }
    else if (!m_BuildCompatible)
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "ABORT: build signature mismatch (SetTransform=%s, ResetBehavior=%s)",
            setTransformMatches ? "ok" : "bad",
            resetBehaviorMatches ? "ok" : "bad");
        SetStatus(details);
    }
    else
    {
        char details[384];
        std::snprintf(details, sizeof(details),
            "Ready - force key 0x%02X; state gate %s; ledge gate %s (raycast %s); cooldown %d ms; breakfall %s (hold %s, request %s)",
            m_ForceKey,
            m_StateGateEnabled ? (m_StateGateRequireValid ? "strict" : "on") : "off",
            m_RequireLedge ? "on" : "off",
            m_RaycastCompatible ? "compatible" : "UNAVAILABLE",
            m_ForceCooldownMs,
            m_BreakfallAfterTeleport ? "on" : "off",
            m_HoldBreakfallWhileHeld ? "on" : "off",
            m_BreakfallRequestCompatible ? "compatible" : "UNAVAILABLE");
        SetStatus(details);
    }

    // Install the deferred-reset game-thread hook. The callback is a no-op
    // unless SafeInjectionOnly is on AND a reset was queued by a teleport.
    g_ExactTeleportDeferred = this;
    try
    {
        g_DeferredResetHook = std::make_unique<AutoAssembleWrapper<DeferredResetHook>>();
        g_DeferredResetHook->Activate();
        m_GameThreadHookInstalled = true;
        Log("Deferred-reset game-thread hook installed at 0x141F3FE3B");
    }
    catch (...)
    {
        m_GameThreadHookInstalled = false;
        Log("WARNING: deferred-reset game-thread hook failed to install; safe injection falls back to direct reset");
    }
}

void ExactPositionTeleportPlugin::OnUpdate()
{
    if (!m_PluginEnabled)
    {
        if (m_ForceActive)
        {
            m_ForceActive = false;
        }
        if (m_PendingBreakfall)
        {
            m_PendingBreakfall = false;
        }
        if (m_ShiftNegationActive)
        {
            m_ShiftNegationActive = false;
        }
        return;
    }

    ServiceShiftNegation();

    // Report a completed deferred (game-thread) reset on this thread, so the
    // hook callback itself stays free of logging/file I/O.
    if (m_DeferredResetDone)
    {
        m_DeferredResetDone = false;
        SetStatus(m_LastResetDispatched
            ? "Deferred HSTE reset executed on game thread (safe injection)"
            : "Deferred HSTE reset failed: player Human was unavailable");
    }

    const bool keyDown = (GetAsyncKeyState(m_ForceKey) & 0x8000) != 0;
    const bool shiftHeld = IsShiftHeld();

    // Shift + CatchLedge key is not a valid CatchLedge input. Track the
    // physical key state below so releasing Shift while the key remains held
    // cannot turn the rejected press into a new valid input.
    if (!m_WaitingForKey && keyDown && !m_PrevKeyDown)
    {
        if (shiftHeld)
        {
            SetStatus("CatchLedge input ignored: Shift is held");
        }
        else
        {
            BeginForceFreefall();
        }
    }

    if (m_ForceActive)
    {
        if (!keyDown)
        {
            ReleaseForce();
        }
        else
        {
            ServiceHeldBreakfall();
            // Continuous re-application of teleport/freefall mechanism while
            // the key is held (after the force cooldown has elapsed). Shift
            // remains a hard blocker for every re-application attempt.
            if (m_ReapplyTeleportWhileHeld && !shiftHeld && (GetTickCount64() >= m_NextForceAllowedAt))
            {
                BeginForceFreefall();
            }
        }
    }

    m_PrevKeyDown = keyDown;

    ProcessPendingBreakfall();
}

void ExactPositionTeleportPlugin::BeginForceFreefall()
{
    if (!m_PluginEnabled)
    {
        SetStatus("CatchLedge is disabled (enable it in the UI)");
        return;
    }

    m_PendingBreakfall = false;

    // Cooldown: repeated teleports/force-freefalls in quick succession are the
    // suspected cause of the mid-air crash, so refuse to fire again until the
    // configured buffer has elapsed.
    const uint64_t now = GetTickCount64();
    if (now < m_NextForceAllowedAt)
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "Force freefall on cooldown (%.0f ms remaining)",
            static_cast<double>(m_NextForceAllowedAt - now));
        SetStatus(details);
        return;
    }

    m_HoldRequestCount = 0;

    if (m_StateGateEnabled && !ProbeStateGate())
    {
        return;
    }

    // State detection buffer: after a valid state is first seen, wait the
    // configured buffer before allowing teleport/freefall.
    if (m_StateGateEnabled && m_StateDetectionBufferMs > 0)
    {
        const uint64_t now = GetTickCount64();
        if (m_StateValidSinceMs == 0 || (now - m_StateValidSinceMs) < static_cast<uint64_t>(m_StateDetectionBufferMs))
        {
            char details[224];
            std::snprintf(details, sizeof(details),
                "Force freefall blocked: state-buffer (%.0f ms remaining; valid since %llu)",
                static_cast<double>(m_StateValidSinceMs ? (static_cast<uint64_t>(m_StateDetectionBufferMs) - (now - m_StateValidSinceMs)) : static_cast<uint64_t>(m_StateDetectionBufferMs)),
                static_cast<unsigned long long>(m_StateValidSinceMs));
            SetStatus(details);
            return;
        }
    }

    if (m_RequireLedge)
    {
        if (!m_RaycastCompatible)
        {
            SetStatus("Force freefall blocked: raycast ledge gate unavailable for this build");
            return;
        }
        if (!ProbeLedgeGate())
        {
            char details[224];
            std::snprintf(details, sizeof(details),
                "Force freefall blocked: no ledge within %.2f m (rays=%d, walls=%d)",
                m_MaxHorizontalReach, m_LastProbeRays, m_LastProbeWalls);
            SetStatus(details);
            return;
        }
    }

    if (!TeleportToExactCurrentPosition())
    {
        return;
    }

    // Arm the cooldown as soon as a teleport was actually performed.
    m_NextForceAllowedAt = GetTickCount64() + static_cast<uint64_t>(m_ForceCooldownMs);

    // Safe injection: reset-only mode (validated stable config) disables
    // the external breakfall-request layer entirely.
    const bool requestBreakfall = m_BreakfallAfterTeleport && !m_SafeInjectionOnly;
    if (!requestBreakfall)
    {
        return;
    }

    if (!m_BreakfallRequestCompatible)
    {
        m_LastBreakfallRequested = false;
        SetStatus(m_LastStatus + "; breakfall request unavailable for this build");
        return;
    }

    // Give the reset/freefall transition a few frames to establish before
    // asking the parkour pipeline to execute breakfall.
    m_BreakfallRequestAt = GetTickCount64() + static_cast<uint64_t>(m_BreakfallDelayMs);
    m_NextHoldRequestAt = m_BreakfallRequestAt;
    m_LastBreakfallRequested = false;
    m_ForceActive = true;

    if (m_HoldBreakfallWhileHeld)
    {
        // The held-key service path delivers (and keeps re-issuing) the request.
        SetStatus(m_LastStatus + "; breakfall held while key is down");
    }
    else
    {
        m_PendingBreakfall = true;
        SetStatus(m_LastStatus + "; breakfall queued");
    }
}

bool ExactPositionTeleportPlugin::ProbeLedgeGate()
{
    if (!m_RaycastCompatible)
    {
        m_LastProbeFound = false;
        SetStatus("Ledge probe skipped: raycast API unavailable for this build");
        return false;
    }

    Entity* player = GetPlayerSafely();
    if (!player)
    {
        m_LastProbeFound = false;
        SetStatus("Ledge probe skipped: player entity is unavailable");
        return false;
    }

    World* world = GetWorldSafely();
    if (!world)
    {
        m_LastProbeFound = false;
        SetStatus("Ledge probe skipped: world is unavailable");
        return false;
    }

    const Vector3f feet = player->GetPosition();
    const LedgeProbeOutcome outcome = ProbeForLedge(
        world, feet,
        m_ProbeDirections, m_MaxHorizontalReach, m_WallProbeHeight,
        m_WallNormalMaxAbsZ, m_LedgeInset, m_MinLedgeRise, m_MaxLedgeRise,
        m_TopNormalMinZ);

    m_LastProbeFound = outcome.found;
    m_LastProbeRays = outcome.raysCast;
    m_LastProbeWalls = outcome.wallsFound;
    m_LastProbeDistance = outcome.bestDistance;
    m_LastProbeRise = outcome.bestLedgeRise;

    if (!outcome.found)
    {
        char details[224];
        std::snprintf(details, sizeof(details),
            "No ledge detected within %.2f m (rays=%d, walls=%d)",
            m_MaxHorizontalReach, outcome.raysCast, outcome.wallsFound);
        SetStatus(details);
        return false;
    }

    char details[256];
    std::snprintf(details, sizeof(details),
        "Ledge detected: wall %.2f m ahead, top %.2f m relative to feet (rays=%d, walls=%d)",
        outcome.bestDistance, outcome.bestLedgeRise,
        outcome.raysCast, outcome.wallsFound);
    SetStatus(details);
    return true;
}

bool ExactPositionTeleportPlugin::ProbeStateGate()
{
    ActiveHumanStates states;
    const bool evaluated = CollectActiveStates(states);

    m_LastStateGateEvaluated = evaluated;
    m_LastStateOnGroundLow = states.onGroundLowProfile;
    m_LastStateOnGroundHigh = states.onGroundHighProfile;
    m_LastStateOnPeakOrLedge = states.onPeakOrLedge;
    m_LastStateWalling = states.walling;
    m_LastStateSafeJumpOrLowering = states.inSafeJumpOrLoweringToHang;
    m_LastStateWallrunUp = states.wallrunUp;

    if (!evaluated)
    {
        m_LastStateGateBlocked = m_StateGateRequireValid;
        if (m_LastStateGateBlocked)
        {
            SetStatus("Force freefall blocked: could not read the player's human states");
        }
        return !m_LastStateGateBlocked;
    }

    const bool inInvalidState =
        states.onGroundLowProfile || states.onGroundHighProfile ||
        states.onPeakOrLedge || states.walling;
    const bool inValidState =
        states.inSafeJumpOrLoweringToHang || states.wallrunUp || states.detachingFromWall;

    if (inInvalidState)
    {
        m_StateValidSinceMs = 0; // invalid state resets the buffer
        m_LastStateGateBlocked = true;
        char details[288];
        std::snprintf(details, sizeof(details),
            "Force freefall blocked by state gate: %s%s%s%s(nodes=%d)",
            states.onGroundLowProfile ? "OnGroundLowProfile " : "",
            states.onGroundHighProfile ? "OnGroundHighProfile " : "",
            states.onPeakOrLedge ? "OnPeakOrLedge " : "",
            states.walling ? "Walling" : "",
            states.nodeCount);
        SetStatus(details);
        return false;
    }

    if (inValidState)
    {
        m_LastStateGateBlocked = false;
        if (m_StateValidSinceMs == 0) m_StateValidSinceMs = GetTickCount64();
        SetStatus(std::string("State gate: valid (") +
            (states.inSafeJumpOrLoweringToHang ? "InSafeJumpOrLoweringToHang " : "") +
            (states.wallrunUp ? "WallrunUp" : "") + ")");
        return true;
    }

    // Neither list. Permissive unless the caller asked to require a known-valid
    // state.
    if (m_StateGateRequireValid)
    {
        m_StateValidSinceMs = 0;
        m_LastStateGateBlocked = true;
        SetStatus("Force freefall blocked: not in a known-valid human state");
        return false;
    }
    if (m_StateValidSinceMs == 0) m_StateValidSinceMs = GetTickCount64();
    m_LastStateGateBlocked = false;
    return true;
}

bool ExactPositionTeleportPlugin::TeleportToExactCurrentPosition()
{
    m_LastTransformApplied = false;
    m_LastResetDispatched = false;

    if (!m_BuildCompatible)
    {
        SetStatus("Teleport skipped: incompatible ACU build");
        return false;
    }

    Entity* player = GetPlayerSafely();
    if (!player)
    {
        SetStatus("Teleport skipped: player entity is unavailable");
        return false;
    }

    // Debug toggle: "teleport before reset" off skips the transform write
    // entirely, so only the HSTE reset runs. Isolates the reset from the
    // teleport when A/B testing.
    if (m_TeleportBeforeReset)
    {
        // Snapshot before writing. The destination is exactly the position that
        // was sampled; no offset or movement is introduced.
        const Vector3f exactPosition = player->GetPosition();
        const Matrix4f exactTransform = player->GetTransform();

        m_LastX = exactPosition.x;
        m_LastY = exactPosition.y;
        m_LastZ = exactPosition.z;

        // Keep the library-proven position write, then also send the unchanged
        // matrix through the native transform setter so this is a real transform
        // update rather than merely a same-value assignment that the compiler
        // could discard.
        player->GetPosition() = exactPosition;
        m_LastTransformApplied = ApplyNativeSetTransform(player, &exactTransform);

        if (!m_LastTransformApplied)
        {
            SetStatus("Teleport failed: native SetTransform raised an exception");
            return false;
        }
    }
    else
    {
        m_LastTransformApplied = false;
        SetStatus("Teleport disabled (debug): reset-only, no transform write");
    }

    // A/B test mode: with the reset disabled, the transform write is a
    // same-value no-op, so nothing in this pipeline can change the game's
    // state. Skip the Human lookup entirely.
    if (!m_ResetBehaviorAfterTeleport)
    {
        m_LastResetDispatched = false;
        char details[224];
        std::snprintf(details, sizeof(details),
            "Transform written at (%.3f, %.3f, %.3f); HSTE reset DISABLED (test)%s",
            m_LastX, m_LastY, m_LastZ,
            m_BreakfallAfterTeleport
                ? " - warning: breakfall requests will fire outside freefall"
                : "");
        SetStatus(details);
        return true;
    }

    // Safe injection: queue the reset for the game-thread hook instead of
    // dispatching it from the Present thread. The hook executes it on the
    // game/simulation thread, at a point the game's own state machine
    // expects mutations (same timing as ACUFixes' own hook contexts).
    if (m_SafeInjectionOnly && m_GameThreadHookInstalled)
    {
        m_DeferredResetRequested = true;
        m_DeferredResetDone = false;
        // Short queue delay so the transform write settles first; the hook
        // fires every frame on the game thread and picks it up after this.
        m_DeferredResetAtMs = GetTickCount64() + 50;

        char details[224];
        std::snprintf(details, sizeof(details),
            "Teleported to exact position (%.3f, %.3f, %.3f); reset QUEUED for game-thread hook",
            m_LastX, m_LastY, m_LastZ);
        SetStatus(details);
        return true;
    }

    // Direct path (safe injection off or hook unavailable): dispatch
    // immediately from this (Present) thread - the historical behavior.
    Human* human = GetPlayerHumanSafely();
    if (!human)
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "Transform applied at (%.3f, %.3f, %.3f), but player Human was unavailable",
            m_LastX, m_LastY, m_LastZ);
        SetStatus(details);
        return false;
    }

    // This is the same HSTE reset used by ACUFixes' debug TeleportPlayer.
    // It lets the game cancel/re-evaluate the current parkour behavior after
    // seeing the transformed position.
    m_LastResetDispatched = DispatchHumanReset(human);
    if (!m_LastResetDispatched)
    {
        SetStatus("Teleport failed: HSTE ResetBehavior raised an exception");
        return false;
    }

    char details[224];
    std::snprintf(details, sizeof(details),
        "Teleported to exact position (%.3f, %.3f, %.3f); reset dispatched",
        m_LastX, m_LastY, m_LastZ);
    SetStatus(details);
    return true;
}

// Runs INSIDE the per-frame game-thread hook (FOV site 0x141F3FE3B).
// Executes the queued HSTE reset on the game/simulation thread. Deliberately
// minimal: no file logging or string building here - the completion status is
// reported by OnUpdate on the Present thread via m_DeferredResetDone.
void ExactPositionTeleportPlugin::ServiceDeferredReset()
{
    if (!m_DeferredResetRequested)
    {
        return;
    }
    if (GetTickCount64() < m_DeferredResetAtMs)
    {
        return;
    }

    m_DeferredResetRequested = false;

    Human* human = GetPlayerHumanSafely();
    if (!human)
    {
        m_DeferredResetDone = true;
        m_LastResetDispatched = false;
        return;
    }

    m_LastResetDispatched = DispatchHumanReset(human);
    m_DeferredResetDone = true;
}

void ExactPositionTeleportPlugin::ServiceHeldBreakfall()
{
    if (!m_BreakfallAfterTeleport || !m_BreakfallRequestCompatible)
    {
        return;
    }

    const uint64_t now = GetTickCount64();
    if (now < m_BreakfallRequestAt)
    {
        return;
    }

    if (m_BreakfallHoldIntervalMs > 0 && now < m_NextHoldRequestAt)
    {
        return;
    }

    m_LastBreakfallRequested = RequestParkourActionSafely(m_BreakfallActionId);
    m_NextHoldRequestAt = now + static_cast<uint64_t>(m_BreakfallHoldIntervalMs);
    ++m_HoldRequestCount;

    if (!m_LastBreakfallRequested)
    {
        SetStatus("Breakfall request raised an exception while held");
    }
    else if (m_HoldRequestCount <= 1)
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "Breakfall action %d requested; will stay applied while the key is held",
            m_BreakfallActionId);
        SetStatus(details);
    }
}

void ExactPositionTeleportPlugin::ReleaseForce()
{
    m_ForceActive = false;
    if (m_HoldRequestCount > 0)
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "Force key released after %d breakfall request(s)", m_HoldRequestCount);
        SetStatus(details);
    }
}

void ExactPositionTeleportPlugin::ProcessPendingBreakfall()
{
    if (!m_PendingBreakfall || GetTickCount64() < m_BreakfallRequestAt)
    {
        return;
    }

    m_PendingBreakfall = false;
    if (!m_BreakfallRequestCompatible)
    {
        m_LastBreakfallRequested = false;
        SetStatus("Breakfall request skipped: incompatible ACU build");
        return;
    }

    m_LastBreakfallRequested = RequestParkourActionSafely(m_BreakfallActionId);
    SetStatus(m_LastBreakfallRequested
        ? "Breakfall action requested through the parkour-action pipeline"
        : "Breakfall action request raised an exception");
}

void ExactPositionTeleportPlugin::ServiceShiftNegation()
{
    if (!m_NegateShiftInSafeJump)
    {
        m_ShiftNegationActive = false;
        return;
    }

    // Cheap physical check first; the state-tree walk only runs while Shift
    // is actually held down.
    if (!IsShiftHeld())
    {
        m_ShiftNegationActive = false;
        return;
    }

    ActiveHumanStates states;
    if (!CollectActiveStates(states) || !states.inSafeJumpOrLoweringToHang)
    {
        m_ShiftNegationActive = false;
        return;
    }

    m_ShiftNegationActive = NegateShiftForGame();
}

void ExactPositionTeleportPlugin::OnImGuiRender()
{
    // Minimal player-facing view: the two main toggles and key binding.
    if (ImGui::Checkbox("Enable CatchLedge", &m_PluginEnabled))
    {
        SaveSettings();
        if (!m_PluginEnabled)
        {
            SetStatus("CatchLedge disabled - plugin will not touch the game");
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Master toggle. When off, the plugin never teleports, resets or requests breakfall.");

    if (ImGui::Checkbox("Debug settings", &m_ShowDebugUi))
    {
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the collapsible debug / parameters section.");

    ImGui::Text("Catch ledge keybind: 0x%02X", m_ForceKey);
    if (m_ForceKey >= 0x20 && m_ForceKey <= 0x7E)
    {
        ImGui::SameLine();
        ImGui::Text("('%c')", static_cast<char>(m_ForceKey));
    }
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey ? "Cancel rebind" : "Rebind"))
    {
        m_WaitingForKey = !m_WaitingForKey;
        if (m_WaitingForKey)
        {
            for (int vk = 1; vk <= 0xFF; ++vk)
                m_CaptureKeyWasDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
    }

    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFF; ++vk)
        {
            const bool keyDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool newlyPressed = keyDown && !m_CaptureKeyWasDown[vk];
            m_CaptureKeyWasDown[vk] = keyDown;
            if (!newlyPressed) continue;
            m_ForceKey = vk; m_WaitingForKey = false;
            m_PrevKeyDown = keyDown; SaveSettings();
            SetStatus("Force key rebound to 0x" + std::to_string(vk));
            break;
        }
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1.0f),
            "Press any keyboard key or mouse button to bind it...");
    }

    // Collapsible debug section: all parameters, sliders, gates, diagnostics.
    if (m_ShowDebugUi && ImGui::TreeNodeEx("Debug / parameters", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Confirm loaded config briefly; shows checkbox / slider values were read.
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
            "Config loaded: reset=%s | safe=%s | buffer=%d ms | reapply=%s",
            m_ResetBehaviorAfterTeleport ? "ON" : "OFF",
            m_SafeInjectionOnly ? "ON" : "OFF",
            m_StateDetectionBufferMs,
            m_ReapplyTeleportWhileHeld ? "ON" : "OFF");

        ImGui::Checkbox("Safe injection method (reset-only / deferred hook)", &m_SafeInjectionOnly);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Safe injection: defers HSTE reset to game-thread hook (not Present thread). Keeps reset; disables external breakfall request.");

        ImGui::Checkbox("Require nearby ledge (raycast gate)", &m_RequireLedge);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When ON, only allows force-free-fall when a vertical wall with a usable horizontal top ledge is detected near the player.");
        if (!m_RaycastCompatible)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Raycast API MISMATCH - gate will block trigger.");
        }

    // Note: shift-negate doesn't fully prevent jitter (tester feedback).
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
        "Note: shift-negate reduces but may not fully eliminate mid-air jitter.");

    if (ImGui::TreeNodeEx("Ledge gate parameters", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderInt("Probe directions", &m_ProbeDirections, 4, 32);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many horizontal directions to scan. Higher = wider coverage, slightly slower.");
        ImGui::SliderFloat("Max horizontal reach", &m_MaxHorizontalReach, 0.25f, 4.0f, "%.2f m");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far horizontally the wall-search ray extends. Raise to detect walls further away.");
        ImGui::SliderFloat("Wall probe height", &m_WallProbeHeight, 0.1f, 2.0f, "%.2f m");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Height above feet for wall ray. Raise for tall walls; lower catches waist-level ledges.");
        ImGui::SliderFloat("Wall normal max |z|", &m_WallNormalMaxAbsZ, 0.05f, 0.9f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How vertical the wall face must be (lower = stricter vertical-only; higher allows sloped surfaces).");
        ImGui::SliderFloat("Ledge inset", &m_LedgeInset, 0.05f, 1.0f, "%.2f m");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far past the wall face to step before scanning downward. Higher catches recessed ledges.");
        ImGui::SliderFloat("Min ledge rise", &m_MinLedgeRise, -3.0f, 2.0f, "%.2f m");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lowest acceptable ledge top above feet (negative = steps down allowed). Raise to require ledge above feet.");
        ImGui::SliderFloat("Max ledge rise", &m_MaxLedgeRise, 0.1f, 5.0f, "%.2f m");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Highest acceptable ledge top above feet. Raise to allow taller ledges.");
        ImGui::SliderFloat("Top normal min z", &m_TopNormalMinZ, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How flat/horizontal the ledge top must be (1.0 = flat; lower allows angled tops).");
        ImGui::TreePop();
    }

    ImGui::Checkbox("Require a known-valid state", &m_StateGateRequireValid);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "When on, the force-freefall is also blocked unless the player is in\n"
            "InSafeJumpOrLoweringToHang or WallrunUp.");
    }

    int cooldownMs = m_ForceCooldownMs;
    if (ImGui::InputInt("Force-freefall cooldown (ms)", &cooldownMs))
    {
        if (cooldownMs < 0) cooldownMs = 0;
        if (cooldownMs > 10000) cooldownMs = 10000;
        m_ForceCooldownMs = cooldownMs;
        SaveSettings();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Minimum time between two force-freefall/teleport attempts.\n"
            "Guards against rapid repeated presses, the suspected crash cause.");
    }

    ImGui::Separator();
    ImGui::Checkbox("Teleport before reset (transform write)", &m_TeleportBeforeReset);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "When ON (default): the exact-position transform write happens, then\n"
            "the HSTE reset. When OFF: the transform write is skipped entirely and\n"
            "only the reset is dispatched - isolates the reset from the teleport\n"
            "when A/B testing which half causes an issue.");
    }

    ImGui::Checkbox("Dispatch HSTE reset after teleport", &m_ResetBehaviorAfterTeleport);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "The reset makes the parkour system cancel the current behavior and\n"
            "re-evaluate the context after the transform write - that re-evaluation\n"
            "is what drops the player into freefall. With it off the teleport is a\n"
            "same-value write: expect NO state change at all.\n"
            "Toggle to A/B test whether the reset itself causes the crash.");
    }

    bool breakfallAfterTeleport = m_BreakfallAfterTeleport;
    if (ImGui::Checkbox("Execute breakfall after teleport", &breakfallAfterTeleport))
    {
        m_BreakfallAfterTeleport = breakfallAfterTeleport;
        if (!m_BreakfallAfterTeleport)
        {
            m_PendingBreakfall = false;
            m_ForceActive = false;
        }
        SaveSettings();
    }

    ImGui::Checkbox("Hold breakfall while forced key is held", &m_HoldBreakfallWhileHeld);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "While the force key is held down, the breakfall action request is\n"
            "re-issued every frame so the breakfall stays applied. Releasing the\n"
            "key stops the requests.");
    }

    if (ImGui::InputInt("Breakfall action id", &m_BreakfallActionId))
    {
        if (m_BreakfallActionId < 0) m_BreakfallActionId = 0;
        if (m_BreakfallActionId > 255) m_BreakfallActionId = 255;
        SaveSettings();
    }

    int breakfallDelayMs = m_BreakfallDelayMs;
    if (ImGui::InputInt("Breakfall request delay (ms)", &breakfallDelayMs))
    {
        if (breakfallDelayMs < 0) breakfallDelayMs = 0;
        if (breakfallDelayMs > 2000) breakfallDelayMs = 2000;
        m_BreakfallDelayMs = breakfallDelayMs;
        SaveSettings();
    }

    int holdIntervalMs = m_BreakfallHoldIntervalMs;
    if (ImGui::InputInt("Held re-issue interval (ms, 0 = every frame)", &holdIntervalMs))
    {
        if (holdIntervalMs < 0) holdIntervalMs = 0;
        if (holdIntervalMs > 2000) holdIntervalMs = 2000;
        m_BreakfallHoldIntervalMs = holdIntervalMs;
        SaveSettings();
    }

    ImGui::Separator();
    ImGui::Text("Build signature: %s", m_BuildCompatible ? "compatible" : "MISMATCH");
    ImGui::Text("Raycast API: %s", m_RaycastCompatible ? "compatible" : "MISMATCH");
    ImGui::Text("Breakfall request pipeline: %s",
        m_BreakfallRequestCompatible ? "compatible" : "UNAVAILABLE");
    ImGui::Text("Game-thread hook: %s",
        m_GameThreadHookInstalled ? "installed (0x141F3FE3B)" : "NOT installed");
    ImGui::Text("Force active (key held): %s", m_ForceActive ? "yes" : "no");
    ImGui::Text("Last ledge probe: %s (%.2f m wall, %.2f m rise)",
        m_LastProbeFound ? "FOUND" : "none",
        m_LastProbeDistance, m_LastProbeRise);
    ImGui::Text("Last probe rays/walls: %d / %d", m_LastProbeRays, m_LastProbeWalls);
    ImGui::Text("State gate: %s",
        !m_LastStateGateEvaluated ? "unread"
        : (m_LastStateGateBlocked ? "BLOCKED" : "allowed"));
    ImGui::Text("  active: %s%s%s%s%s%s",
        m_LastStateOnGroundLow ? "GroundLow " : "",
        m_LastStateOnGroundHigh ? "GroundHigh " : "",
        m_LastStateOnPeakOrLedge ? "PeakOrLedge " : "",
        m_LastStateWalling ? "Walling " : "",
        m_LastStateSafeJumpOrLowering ? "SafeJump/Lowering " : "",
        m_LastStateWallrunUp ? "WallrunUp" : "");
    ImGui::Text("Last transform applied: %s", m_LastTransformApplied ? "yes" : "no");
    ImGui::Text("Last HSTE reset dispatched: %s", m_LastResetDispatched ? "yes" : "no");
    ImGui::Text("Shift negation (mid-jump): %s", m_ShiftNegationActive ? "ACTIVE" : (m_NegateShiftInSafeJump ? "idle" : "off"));
    ImGui::Text("Held breakfall requests: %d", m_HoldRequestCount);
    ImGui::Text("Last breakfall request: %s",
        m_LastBreakfallRequested ? "dispatched" : (m_PendingBreakfall ? "pending" : "no"));
    ImGui::Text("Last position: %.3f, %.3f, %.3f", m_LastX, m_LastY, m_LastZ);
    ImGui::TextWrapped("Settings file: %s", m_IniPath.c_str());

    if (ImGui::Button("Probe ledge now (no teleport)"))
    {
        ProbeLedgeGate();
    }

    ImGui::TreePop(); // close "Debug / parameters"
    } // end debug section
}

void ExactPositionTeleportPlugin::LoadSettings()
{
    std::ifstream file(m_IniPath);
    if (!file)
    {
        return;
    }

    auto parseFloat = [](const std::string& value, float fallback) -> float {
        try { return std::stof(value); }
        catch (...) { return fallback; }
    };
    auto parseInt = [](const std::string& value, int fallback) -> int {
        try { return std::stoi(value); }
        catch (...) { return fallback; }
    };
    auto parseBool = [](const std::string& value, bool fallback) -> bool {
        try { return std::stoi(value) != 0; }
        catch (...) { return fallback; }
    };

    std::string line;
    while (std::getline(file, line))
    {
        const size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos)
        {
            line.erase(comment);
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }

        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        auto trim = [](std::string& text) {
            const size_t first = text.find_first_not_of(" \t\r\n");
            const size_t last = text.find_last_not_of(" \t\r\n");
            text = (first == std::string::npos) ? std::string() : text.substr(first, last - first + 1);
        };
        trim(key);
        trim(value);

        if (key == "ForceKey")
        {
            try
            {
                const int hexKey = std::stoi(value, nullptr, 16);
                if (hexKey >= 1 && hexKey <= 0xFF)
                {
                    m_ForceKey = hexKey;
                }
            }
            catch (...) {}
        }
        else if (key == "BreakfallAfterTeleport") { m_BreakfallAfterTeleport = parseBool(value, m_BreakfallAfterTeleport); }
        else if (key == "HoldBreakfallWhileHeld") { m_HoldBreakfallWhileHeld = parseBool(value, m_HoldBreakfallWhileHeld); }
        else if (key == "BreakfallActionId")
        {
            const int id = parseInt(value, m_BreakfallActionId);
            if (id >= 0 && id <= 255) { m_BreakfallActionId = id; }
        }
        else if (key == "BreakfallDelayMs")
        {
            const int delay = parseInt(value, m_BreakfallDelayMs);
            if (delay >= 0 && delay <= 2000) { m_BreakfallDelayMs = delay; }
        }
        else if (key == "BreakfallHoldIntervalMs")
        {
            const int interval = parseInt(value, m_BreakfallHoldIntervalMs);
            if (interval >= 0 && interval <= 2000) { m_BreakfallHoldIntervalMs = interval; }
        }
        else if (key == "ResetBehaviorAfterTeleport") { m_ResetBehaviorAfterTeleport = parseBool(value, m_ResetBehaviorAfterTeleport); }
        else if (key == "NegateShiftInSafeJump") { m_NegateShiftInSafeJump = parseBool(value, m_NegateShiftInSafeJump); }
        else if (key == "StateGateEnabled") { m_StateGateEnabled = parseBool(value, m_StateGateEnabled); }
        else if (key == "StateGateRequireValid") { m_StateGateRequireValid = parseBool(value, m_StateGateRequireValid); }
        else if (key == "ForceCooldownMs")
        {
            const int cooldown = parseInt(value, m_ForceCooldownMs);
            if (cooldown >= 0 && cooldown <= 10000) { m_ForceCooldownMs = cooldown; }
        }
        else if (key == "SafeInjectionOnly") { m_SafeInjectionOnly = parseBool(value, m_SafeInjectionOnly); }
        else if (key == "PluginEnabled") { m_PluginEnabled = parseBool(value, m_PluginEnabled); }
        else if (key == "ShowDebugUi") { m_ShowDebugUi = parseBool(value, m_ShowDebugUi); }
        else if (key == "TeleportBeforeReset") { m_TeleportBeforeReset = parseBool(value, m_TeleportBeforeReset); }
        else if (key == "StateDetectionBufferMs")
        {
            const int buf = parseInt(value, m_StateDetectionBufferMs);
            if (buf >= 0 && buf <= 2000) { m_StateDetectionBufferMs = buf; }
        }
        else if (key == "ReapplyTeleportWhileHeld") { m_ReapplyTeleportWhileHeld = parseBool(value, m_ReapplyTeleportWhileHeld); }
        else if (key == "RequireLedge") { m_RequireLedge = parseBool(value, m_RequireLedge); }
        else if (key == "ProbeDirections")
        {
            const int dirs = parseInt(value, m_ProbeDirections);
            if (dirs >= 1 && dirs <= 64) { m_ProbeDirections = dirs; }
        }
        else if (key == "MaxHorizontalReach") { m_MaxHorizontalReach = parseFloat(value, m_MaxHorizontalReach); }
        else if (key == "WallProbeHeight") { m_WallProbeHeight = parseFloat(value, m_WallProbeHeight); }
        else if (key == "WallNormalMaxAbsZ") { m_WallNormalMaxAbsZ = parseFloat(value, m_WallNormalMaxAbsZ); }
        else if (key == "LedgeInset") { m_LedgeInset = parseFloat(value, m_LedgeInset); }
        else if (key == "MinLedgeRise") { m_MinLedgeRise = parseFloat(value, m_MinLedgeRise); }
        else if (key == "MaxLedgeRise") { m_MaxLedgeRise = parseFloat(value, m_MaxLedgeRise); }
        else if (key == "TopNormalMinZ") { m_TopNormalMinZ = parseFloat(value, m_TopNormalMinZ); }
    }
}

void ExactPositionTeleportPlugin::SaveSettings()
{
    try
    {
        const std::filesystem::path path(m_IniPath);
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream file(path, std::ios::trunc);
        if (!file)
        {
            return;
        }

        file << "// ExactPositionTeleport settings\n"
             << "// ForceKey is a Windows virtual-key code in hexadecimal (54 = T).\n"
             << "ForceKey=" << std::hex << m_ForceKey << std::dec << "\n"
             << "BreakfallAfterTeleport=" << (m_BreakfallAfterTeleport ? 1 : 0) << "\n"
             << "HoldBreakfallWhileHeld=" << (m_HoldBreakfallWhileHeld ? 1 : 0) << "\n"
             << "BreakfallActionId=" << m_BreakfallActionId << "\n"
             << "BreakfallDelayMs=" << m_BreakfallDelayMs << "\n"
             << "BreakfallHoldIntervalMs=" << m_BreakfallHoldIntervalMs << "\n"
             << "ResetBehaviorAfterTeleport=" << (m_ResetBehaviorAfterTeleport ? 1 : 0) << "\n"
             << "NegateShiftInSafeJump=" << (m_NegateShiftInSafeJump ? 1 : 0) << "\n"
             << "StateGateEnabled=" << (m_StateGateEnabled ? 1 : 0) << "\n"
             << "StateGateRequireValid=" << (m_StateGateRequireValid ? 1 : 0) << "\n"
             << "ForceCooldownMs=" << m_ForceCooldownMs << "\n"
             << "RequireLedge=" << (m_RequireLedge ? 1 : 0) << "\n"
             << "ProbeDirections=" << m_ProbeDirections << "\n"
             << "MaxHorizontalReach=" << m_MaxHorizontalReach << "\n"
             << "WallProbeHeight=" << m_WallProbeHeight << "\n"
             << "WallNormalMaxAbsZ=" << m_WallNormalMaxAbsZ << "\n"
             << "LedgeInset=" << m_LedgeInset << "\n"
             << "MinLedgeRise=" << m_MinLedgeRise << "\n"
             << "MaxLedgeRise=" << m_MaxLedgeRise << "\n"
              << "TopNormalMinZ=" << m_TopNormalMinZ << "\n"
              << "SafeInjectionOnly=" << (m_SafeInjectionOnly ? 1 : 0) << "\n"
              << "StateDetectionBufferMs=" << m_StateDetectionBufferMs << "\n"
              << "ReapplyTeleportWhileHeld=" << (m_ReapplyTeleportWhileHeld ? 1 : 0) << "\n"
              << "PluginEnabled=" << (m_PluginEnabled ? 1 : 0) << "\n"
              << "ShowDebugUi=" << (m_ShowDebugUi ? 1 : 0) << "\n"
              << "TeleportBeforeReset=" << (m_TeleportBeforeReset ? 1 : 0) << "\n";
    }
    catch (...)
    {
    }
}

bool ExactPositionTeleportPlugin::EnsureIniFile()
{
    auto tryUsePath = [this](const std::string& path) -> bool
    {
        try
        {
            const std::filesystem::path fsPath(path);
            if (!fsPath.parent_path().empty())
            {
                std::filesystem::create_directories(fsPath.parent_path());
            }

            std::error_code fileError;
            const bool exists = std::filesystem::exists(fsPath, fileError);
            if (!exists || (!fileError && std::filesystem::file_size(fsPath, fileError) == 0))
            {
                if (!WriteDefaultIni(fsPath))
                {
                    return false;
                }
            }
            else
            {
                // Confirm that the existing file is writable without erasing
                // the user's settings.
                std::ofstream probe(fsPath, std::ios::app);
                if (!probe)
                {
                    return false;
                }
            }

            m_IniPath = path;
            return true;
        }
        catch (...)
        {
            return false;
        }
    };

    if (tryUsePath(ResolvePrimaryIniPath()))
    {
        return true;
    }

    // A protected game-install directory is a common reason for the primary
    // path to fail. Use the standard ACU writable documents directory then.
    if (tryUsePath(GetDocumentsIniPath()))
    {
        return true;
    }

    m_IniPath = ResolvePrimaryIniPath();
    return false;
}

std::string ExactPositionTeleportPlugin::ResolvePrimaryIniPath() const
{
    char executablePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        const std::filesystem::path path(executablePath);
        return (path.parent_path() / "ExactPositionTeleport.ini").string();
    }

    return GetDocumentsIniPath();
}

void ExactPositionTeleportPlugin::SetStatus(const std::string& status)
{
    m_LastStatus = status;
    Log(status);
}

void ExactPositionTeleportPlugin::Log(const std::string& line) const
{
    const std::string text = "[ExactPositionTeleport] " + line + "\n";
    OutputDebugStringA(text.c_str());

    try
    {
        const std::filesystem::path path(GetLogPath());
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream file(path, std::ios::app);
        if (file)
        {
            file << text;
        }
    }
    catch (...)
    {
    }
}

std::string ExactPositionTeleportPlugin::GetLogPath() const
{
    char documents[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, documents)))
    {
        return std::string(documents) + "\\Assassin's Creed Unity\\ExactPositionTeleport.log";
    }
    return "ExactPositionTeleport.log";
}
