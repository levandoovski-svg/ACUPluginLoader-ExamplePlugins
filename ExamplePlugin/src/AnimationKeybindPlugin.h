#pragma once

#include "ACU/ManagedPtrs/ManagedPtrs.h"
#include "ACU/Animation.h"
#include "AnimationTools/AnimationPicker.h"

struct AnimationSlot
{
    int keyCode = VK_F9;
    bool waitingForKey = false;
    bool enable = true;
    std::string label;
    ACU::StrongRef<Animation> anim;
    uint64 animHandle = 0;
};

class AnimationKeybindPlugin
{
public:
    void OnBeforeActivate();
    void OnBeforeDeactivate();
    void OnUpdate();
    void OnImGuiRender();

private:
    static bool IsRisingEdgePressed(int vkCode);
    void LoadSettings();
    void SaveSettings();
    void PickAnimation(int slotIdx);

    std::vector<AnimationSlot> m_slots;
    AnimationPicker m_Picker;
};
