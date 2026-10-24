#include "pch.h"
#include "BreakfallForcePlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_breakfallForce.OnImGuiRender();
    }
    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_breakfallForce.OnUpdate();
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
        g_breakfallForce.LoadSettings();
        return true;
    }

    BreakfallForcePlugin g_breakfallForce;
} g_thisPlugin;
