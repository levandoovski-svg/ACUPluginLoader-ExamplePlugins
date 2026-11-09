#include "pch.h"
#include "SwingToSwingWidenPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

// SwingToSwingWiden is the only gameplay plugin in this DLL.
class SwingToSwingWidenPluginMain : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_swingToSwingWiden.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_swingToSwingWiden.OnUpdate();
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
        g_swingToSwingWiden.OnBeforeActivate();
        return true;
    }

    SwingToSwingWidenPlugin g_swingToSwingWiden;
} g_thisPlugin;
