#pragma once

// ============================================================================
//  ForcedSidehopPlugin
//  ----------------------------------------------------------------------------
//  Rebuild of the "forced sidehop" feature from the third-party
//  ACU_ParkourPatches.dll (reverse-engineered: FUN_18006d510 + FUN_18006cf30).
//
//  How the original works (static RE findings, see D:\Games\parkourpatchdecompiled.md):
//    * A callback is registered into the parkour action "sort & select" stage
//      (GenericHooksInParkourFiltering).
//    * While the trigger keys are held, the first candidate of action type
//      0x41 (onWallNormalClimb) or 0x44 (onWallTurnCornerOuter) is rewritten
//      in place:
//        - its fancy vtable is replaced (0x143A13130) -> the sidehop behavior
//        - minCosAngle        = 0.7071068  (sqrt(2)/2)
//        - expectedDuration   = 0.5, horizSpeed = 4.0, vertSpeed = 9.0
//        - curveAllowedMax    = 2.0
//        - dir80 (+0x80)      = perpendicular of directionDestFacingOut,
//                               mirrored by which direction key is held
//        - locationAnchorSrc  += dir * 0.5
//        - locationAnchorDest = src' + perp * 3.5 + dir * 0.5 (z/w copied)
//        - heightDifference = 0, horizontalDifference = distance = len * 3.5
//        - fitness floored at 1.0 so it survives the fitness filter
//    * If no gate holds, the callback declines and vanilla selection runs.
//
//  This rebuild keeps the original math but makes every parameter tunable
//  through an ImGui debug panel, adds live telemetry, and a Reset-to-Defaults
//  button. Settings persist to an INI next to the other plugins' files.
// ============================================================================

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class AvailableParkourAction;

// Original hard-coded values from ACU_ParkourPatches.dll, exposed as tunables.
struct ForcedSidehopParams
{
    // -- Trigger conditions ------------------------------------------------
    int  hotkey                  = 0x05;  // VK_XBUTTON2 - hold to trigger
    int  dirKeyA                 = 0x20;  // VK_SPACE    - hop perpendicular A
    int  dirKeyB                 = 0xA2;  // VK_LCONTROL - hop perpendicular B
                                          //   (match your in-game crouch bind)
    bool requireModifier         = false; // original: optional modifier gate
    int  modifierKey             = 0x12;  // VK_MENU (Alt)
    bool blockWhenMouseButtonsHeld = true;// original: LMB/RMB held -> decline

    // -- Target action types (EnumParkourAction) ---------------------------
    bool matchOnWallNormalClimb   = true; // 65  (0x41)
    bool matchOnWallTurnCornerOuter = true; // 68 (0x44)
    std::vector<int> extraActionTypes;    // any additional TypeInts to match

    // -- Direction source --------------------------------------------------
    int directionOverride = 0;            // 0 = from keys, 1 = force A, 2 = force B

    // -- Mutation parameters (defaults = reverse-engineered constants) -----
    uintptr_t vtableAddress        = 0x143A13130; // replacement fancy vtable
    float reachMultiplier          = 3.5f;  // hop reach, horizontalDifference, distance
    float startOffsetMul           = 0.5f;  // src += dir * this
    float destForwardMul           = 0.5f;  // dest = src' + hop + dir * this
    float weightFloor              = 1.0f;  // fitness floor (original: if |f| < 1 -> 1)
    float minCosAngle              = 0.7071068f;      // +0x1E8
    float expectedDurationMin      = 0.5f;            // +0x210
    float expectedHorizontalSpeed  = 4.0f;            // +0x218
    float expectedVerticalSpeed    = 9.0f;            // +0x220
    float curveAllowedRangeMax     = 2.0f;            // +0x230
    bool  forceHeightDiff          = true;            // original always writes 0
    float forcedHeightDiff         = 0.0f;            //   to heightDifference (+0xC0)

    // -- Selection enforcement ---------------------------------------------
    // The original DLL's framework used the callback's return value as the
    // chosen action. This codebase's GPH dispatcher discards return values,
    // so "chosen" must be expressed through the candidate list itself:
    //   0 = fitness floor only (original-faithful)
    //   1 = set fitness to boostFitness (this framework's equivalent)
    //   2 = both
    int   selectionMode   = 1;
    float boostFitness    = 9999.0f;
    bool  removeOtherActions = false;     // prune every candidate but the chosen one
};

class ForcedSidehopPlugin
{
public:
    void Init();        // load INI
    void OnUpdate();    // per-frame: key rebinds + hook lifecycle
    void OnImGuiRender();

    // Runs on the game thread inside the parkour filtering hook.
    AvailableParkourAction* ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions);

private:
    // -- Live telemetry written from the game thread, read by the UI -------
    struct Telemetry
    {
        std::atomic<long long> cyclesSeen{ 0 };
        std::atomic<long long> timesTriggered{ 0 };
        std::atomic<int>  lastCandidateCount{ 0 };
        std::atomic<int>  lastMatchedIndex{ -1 };
        std::atomic<int>  lastMatchedType{ -1 };
        std::atomic<int>  lastFitnessBeforeAsInt{ 0 }; // float bits, avoids atomic<float> ABI doubt
        std::atomic<int>  lastFitnessAfterAsInt{ 0 };
        std::atomic<bool> lastCycleMatched{ false };
        std::atomic<bool> gateHotkey{ false };
        std::atomic<bool> gateDirection{ false };
        std::atomic<bool> gateModifier{ false };
        std::atomic<bool> gateMouse{ false };
        std::atomic<int>  lastDirectionKey{ 0 }; // 0 = none, 1 = A, 2 = B
    };
    Telemetry m_Tel;

    static bool IsRisingEdgePressed(int vkCode) { return (GetAsyncKeyState(vkCode) & 1) != 0; }

    bool IsMatchType(int typeInt) const;
    void RefreshExtraTypesBuf();
    void ResetToDefaults();
    void ActivateHook();
    void DeactivateHook();

    // -- Settings persistence ----------------------------------------------
    static std::string GetIniPath();
    void LoadSettings();
    void SaveSettings();

    // -- State -------------------------------------------------------------
    ForcedSidehopParams m_Params;
    bool m_PluginEnabled = false;

    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_HookActivator;
    bool m_HookActive = false;
    std::atomic<bool> m_HookRefused{ false }; // activation refused: sort&select site already patched by another plugin

    // Key rebind state
    enum class KeySlot { None, Hotkey, DirA, DirB, Modifier };
    KeySlot m_WaitingKey = KeySlot::None;

    char m_ExtraTypesBuf[128] = "";
};
