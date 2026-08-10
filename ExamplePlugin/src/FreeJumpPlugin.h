#pragma once

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/SmallArray.h"
#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include <string>
#include <vector>
#include <memory>

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
    static bool IsRisingEdgePressed(int vkCode);

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

    // Overrides
    std::vector<ParkourActionOverride> m_Overrides;

    // Ability set management for mouse lock
    struct SavedAbilityFlags
    {
        uint8_t byte22 = 0, byte29 = 0, byte2A = 0;
    } m_SavedFlags;
    bool m_FlagsSaved = false;
    void ApplyMouseLock(bool enable);
    void SaveCurrentFlags();
    void RestoreFlags();
    static class AssassinAbilitySet* GetActiveAbilitySet();
};
