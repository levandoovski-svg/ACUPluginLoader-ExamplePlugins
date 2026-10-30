#include "pch.h"
#include "AnimationLoopFixPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class AnimationLoopFixEntry : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_Plugin.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_Plugin.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 1, 0, 0);
    }

    // Game-independent only: INI path, defaults, log.
    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
        g_Plugin.OnBeforeActivate();
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& /*pluginLoader*/) override
    {
        return g_Plugin.OnWhenCodePatchesAreSafeToApply();
    }

    AnimationLoopFixPlugin g_Plugin;
} g_thisPlugin;
