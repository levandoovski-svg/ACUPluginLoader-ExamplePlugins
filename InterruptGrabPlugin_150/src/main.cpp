#include "pch.h"
#include "InterruptGrabPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

// InterruptGrabPlugin_150 is the only plugin in this DLL.
class InterruptGrabPluginMain : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_interruptGrab.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_interruptGrab.OnUpdate();
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
        g_interruptGrab.OnBeforeActivate();
        return true;
    }

    InterruptGrabPlugin g_interruptGrab;
} g_thisPlugin;
