#include "pch.h"
#include "FreeJumpPlugin.h"
#include "ParryAssassinPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_freeJump.OnImGuiRender();
        g_parryAssassin.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_freeJump.OnUpdate();
        g_parryAssassin.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 6, 0, 0);
    }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        g_freeJump.OnBeforeActivate();
        g_parryAssassin.LoadSettings();
        return true;
    }

    FreeJumpPlugin g_freeJump;
    ParryAssassinPlugin g_parryAssassin;
} g_thisPlugin;
