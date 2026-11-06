#include "pch.h"
#include "FancyVTableSwapPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class FancyVTableSwapPluginHost : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override { m_plugin.OnImGuiRender(); }
    virtual void EveryFrameEvenWhenMenuIsClosed() override { m_plugin.OnUpdate(); }
    virtual uint64 GetThisPluginVersion() override { return MAKE_VERSION_NUMBER_UINT64(0, 1, 0, 0); }
    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        (void)pluginLoader;
        m_plugin.OnBeforeActivate();
        return true;
    }
    FancyVTableSwapPlugin m_plugin;
} g_thisPlugin;
