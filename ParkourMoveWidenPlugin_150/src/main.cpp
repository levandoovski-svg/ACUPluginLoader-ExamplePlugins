#include "pch.h"
#include "ParkourMoveWidenPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

// Parkour move widen is the only plugin in this DLL.
class ParkourMoveWidenPluginMain : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_parkourMoveWiden.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_parkourMoveWiden.OnUpdate();
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
        g_parkourMoveWiden.OnBeforeActivate();
        return true;
    }

    ParkourMoveWidenPlugin g_parkourMoveWiden;
} g_thisPlugin;
