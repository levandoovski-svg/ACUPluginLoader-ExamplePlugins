#include "pch.h"
#include "ExactPositionTeleportPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        // Keep the bound force key active even while the loader menu is open.
        // The rising-edge guard makes this safe if the loader also invokes
        // the closed-menu callback during the same frame.
        g_exactPositionTeleport.OnUpdate();
        g_exactPositionTeleport.OnImGuiRender();
    }
    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_exactPositionTeleport.OnUpdate();
    }
    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 3, 1, 0);
    }
    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
    }
    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        g_exactPositionTeleport.Init();
        return true;
    }

    ExactPositionTeleportPlugin g_exactPositionTeleport;
} g_thisPlugin;
