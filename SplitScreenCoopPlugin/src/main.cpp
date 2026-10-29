#include "pch.h"
#include "SplitScreenCoopPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class SplitScreenCoopEntry : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_coop.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_coop.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 1, 0, 0);
    }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
        // Game-independent only: INI + log + paths.
        g_coop.OnBeforeActivate();
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        return g_coop.OnWhenCodePatchesAreSafeToApply(pluginLoader);
    }

    SplitScreenCoopPlugin g_coop;
} g_thisPlugin;
