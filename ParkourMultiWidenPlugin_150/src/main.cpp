#include "pch.h"
#include "ParkourMultiWidenPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

// Multi-move widen is the only plugin in this DLL.
class ParkourMultiWidenPluginMain : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_parkourMultiWiden.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_parkourMultiWiden.OnUpdate();
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
        g_parkourMultiWiden.OnBeforeActivate();
        return true;
    }

    ParkourMultiWidenPlugin g_parkourMultiWiden;
} g_thisPlugin;
