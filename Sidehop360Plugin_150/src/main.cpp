#include "pch.h"
#include "Sidehop360Plugin.h"
#include "Common_Plugins/Common_PluginSide.h"

// Sidehop360 is the only gameplay plugin in this DLL.
class Sidehop360PluginMain : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_sidehop360.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_sidehop360.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 5, 0, 0);
    }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        g_sidehop360.OnBeforeActivate();
        return true;
    }

    Sidehop360Plugin g_sidehop360;
} g_thisPlugin;
