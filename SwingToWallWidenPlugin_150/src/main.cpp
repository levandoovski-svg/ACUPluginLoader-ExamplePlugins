#include "pch.h"
#include "SwingToWallWidenPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

// SwingToWallWiden is the only gameplay plugin in this DLL.
class SwingToWallWidenPluginMain : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_swingToWallWiden.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_swingToWallWiden.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 1, 0, 0);
    }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        g_swingToWallWiden.OnBeforeActivate();
        return true;
    }

    SwingToWallWidenPlugin g_swingToWallWiden;
} g_thisPlugin;
