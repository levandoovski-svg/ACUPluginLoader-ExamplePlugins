#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"

class AvailableParkourAction;
enum class EnumParkourAction : int;

struct ParkourActionOverride
{
    int keyCode = VK_F9;
    std::vector<int> actionTypes = { 50 };
    float fitnessMultiplier = 9999.0f;
    bool removeOtherActions = true;
    bool enable = true;
    bool toggleMode = false;
    bool toggleState = false;
    bool waitingForKey = false;
};

class FreeJumpPlugin
{
public:
    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();

    AvailableParkourAction* ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions);
private:
    void UpdateKeyState(int vkCode);
    void UpdateAllKeyStates();
    bool WasKeyPressed(int vkCode) const;

    void LoadSettings();
    void SaveSettings();

    // GenericHooksInParkourFiltering callback system
    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_HookActivator;
    bool m_HookActive = false;

    // Basic features
    int  m_ToggleKey         = VK_XBUTTON1;
    bool m_WaitingForKey     = false;
    bool m_SnapDirectionMode = false;
    int  m_SnapDirectionKey  = VK_F8;
    bool m_WaitingForSnapKey = false;
    bool m_MouseLockEnabled  = false;
    bool m_LastMouseLockEnabled = false;

    // Reliable Win32 rising-edge state. The low bit of GetAsyncKeyState is
    // only "pressed since the previous call" and is not reliable for this.
    std::array<bool, 256> m_PreviousKeyDown{};
    std::array<bool, 256> m_KeyPressedThisFrame{};
    std::array<bool, 256> m_KeyStateUpdatedThisFrame{};

    // Overrides
    std::vector<ParkourActionOverride> m_Overrides;

    // Ability set management for mouse lock
    struct SavedAbilityFlags
    {
        uint8_t byte22 = 0, byte29 = 0, byte2A = 0;
    } m_SavedFlags;
    bool m_FlagsSaved = false;
    bool ApplyMouseLock(bool enable);
    bool SaveCurrentFlags();
    bool RestoreFlags();
    static class AssassinAbilitySet* GetActiveAbilitySet();
};
