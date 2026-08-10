#pragma once

#include "Common_Plugins/Common_PluginSide.h"
#include <string>

class FreeJumpPlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual uint64 GetThisPluginVersion() override;
    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override;
    virtual void EveryFrameWhenMenuIsOpen() override;
    virtual void EveryFrameEvenWhenMenuIsClosed() override;

private:
    void AlignPlayerFacingToCamera();
    void LoadSettings();
    void SaveSettings();

    int  m_ToggleKey       = VK_XBUTTON1;
    bool m_WaitingForKey   = false;

    bool  m_SideEjectEnabled = true;
    bool  m_PrevSpaceDown    = false;
};
