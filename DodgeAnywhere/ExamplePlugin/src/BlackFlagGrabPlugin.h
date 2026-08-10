#pragma once

#include "ParkourDebugging/GenericHooksInParkourFiltering.h"
#include "ACU/SmallArray.h"
#include <string>

class AvailableParkourAction;
enum class EnumParkourAction : int;

class BlackFlagGrabPlugin
{
public:
    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();

private:
    AvailableParkourAction* ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions);
    AvailableParkourAction* ChooseAfterSorting(SmallArray<AvailableParkourAction*>& actions, AvailableParkourAction* selectedByGame);
    static bool IsRisingEdgePressed(int vkCode);

    void LoadSettings();
    void SaveSettings();

    ParkourCallbacks m_Callbacks;
    std::shared_ptr<SharedHookActivator> m_HookActivator;
    std::shared_ptr<SharedHookActivator> m_HookActivatorCreation;
    bool m_HookActive = false;

    bool m_PluginEnabled  = false;   // master enable/disable (checkbox)
    int  m_ToggleKey      = VK_F6;
    bool m_WaitingForKey  = false;
    bool m_HoldMode       = false;   // false = toggle mode, true = hold mode
    bool m_ToggleState    = false;   // current state for toggle mode
};