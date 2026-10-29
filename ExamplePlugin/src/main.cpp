#include "pch.h"
#include "FreeJumpPlugin.h"
#include "ParryAssassinPlugin.h"
#include "AllowSmokeAssassinatePlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_freeJump.OnImGuiRender();
        g_parryAssassin.OnImGuiRender();
        g_allowSmoke.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_freeJump.OnUpdate();
        g_parryAssassin.OnUpdate();
        g_allowSmoke.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 7, 0, 0);
    }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        g_freeJump.OnBeforeActivate();
        g_parryAssassin.LoadSettings();
        g_allowSmoke.LoadSettings();
        return true;
    }

    FreeJumpPlugin g_freeJump;
    ParryAssassinPlugin g_parryAssassin;
    AllowSmokeAssassinatePlugin g_allowSmoke;
} g_thisPlugin;
