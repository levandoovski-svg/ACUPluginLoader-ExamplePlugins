#include "pch.h"
#include "ExactPositionTeleportPlugin.h"
#include "FreeJumpPlugin.h"
#include "ParryAssassinPlugin.h"
#include "ForcedSidehopPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        // Keep plugin updates active while the loader menu is open. The
        // rising-edge guards make duplicate calls from the closed-menu path safe.
        g_exactPositionTeleport.OnUpdate();
        g_exactPositionTeleport.OnImGuiRender();
        g_FreeJump.OnUpdate();
        g_FreeJump.OnImGuiRender();
        g_parryAssassin.OnImGuiRender();
        g_forcedSidehop.OnUpdate();
        g_forcedSidehop.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_exactPositionTeleport.OnUpdate();
        g_FreeJump.OnUpdate();
        g_parryAssassin.OnUpdate();
        g_forcedSidehop.OnUpdate();
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
        g_FreeJump.OnBeforeActivate();
        g_parryAssassin.LoadSettings();
        g_forcedSidehop.Init();
        return true;
    }

    ExactPositionTeleportPlugin g_exactPositionTeleport;
    FreeJumpPlugin            g_FreeJump;
    ParryAssassinPlugin       g_parryAssassin;
    ForcedSidehopPlugin       g_forcedSidehop;
} g_thisPlugin;
