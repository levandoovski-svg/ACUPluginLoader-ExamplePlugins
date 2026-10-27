#include "pch.h"
#include "ExactPositionTeleportPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        // Keep the ExactPositionTeleport plugin active while the loader menu is
        // open. Its rising-edge guard makes duplicate calls from the
        // closed-menu path safe.
        g_exactPositionTeleport.OnUpdate();
        g_exactPositionTeleport.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_exactPositionTeleport.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 3, 3, 0);
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
