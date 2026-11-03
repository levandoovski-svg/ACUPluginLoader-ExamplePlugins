#include "pch.h"
#include "FreeJumpPlugin_151.h"
#include "Common_Plugins/Common_PluginSide.h"

// FreeJumpPlugin is the only gameplay plugin in this DLL. All other *Plugin.cpp
// files in src/ are inactive examples excluded from ExamplePlugin.vcxproj.
class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_freeJump.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_freeJump.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 16, 0, 0);
    }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        g_freeJump.OnBeforeActivate();
        return true;
    }

    FreeJumpPlugin_151 g_freeJump;
} g_thisPlugin;
