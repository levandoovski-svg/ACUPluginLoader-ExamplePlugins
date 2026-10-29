#include "pch.h"
#include "ExactPositionTeleportPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

// Dedicated plugin entry point for the ExactPositionTeleportPlugin build.
// Mirrors src/main.cpp (the FreeJumpPlugin entry) but instantiates
// ExactPositionTeleportPlugin instead, so a standalone
// ExactPositionTeleportPlugin.dll can be produced without touching the root
// ExamplePlugin.vcxproj. Built by
// .github/workflows/build-exactteleport.yml.

class ExamplePlugin_ExactTeleport : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_exactTeleport.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_exactTeleport.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 1, 4, 0);
    }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
        // ExactPositionTeleportPlugin::Init() resolves the INI path AND
        // installs the deferred-reset code patch (AutoAssemblerKinda hook at
        // 0x141F3FE3B). Per the AGENTS.md lifecycle rules the patching must
        // happen from the code-patches stage below, so the INI loading runs
        // there as well rather than being split across the two stages.
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        g_exactTeleport.Init();
        return true;
    }

    ExactPositionTeleportPlugin g_exactTeleport;
} g_thisPlugin;
