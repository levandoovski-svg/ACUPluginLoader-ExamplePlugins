#include "pch.h"
#include "FailedWallRunPlugin.h"
#include "Common_Plugins/Common_PluginSide.h"

// Dedicated plugin entry point for the FailedWallRunPlugin build.
// Mirrors src/main.cpp (the FreeJumpPlugin entry) but instantiates
// FailedWallRunPlugin instead, so a standalone FailedWallRunPlugin.dll
// can be produced without touching the root ExamplePlugin.vcxproj.

class ExamplePlugin_FailedWallRun : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        g_failedWallRun.OnImGuiRender();
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        g_failedWallRun.OnUpdate();
    }

    virtual uint64 GetThisPluginVersion() override
    {
        return MAKE_VERSION_NUMBER_UINT64(0, 1, 0, 0);
    }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
        // Configuration loading is game-independent (AGENTS.md lifecycle rule):
        // INI read belongs in this stage, before any code patches are applied.
        g_failedWallRun.LoadSettings();
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        // FailedWallRunPlugin installs its hooks lazily from OnUpdate()
        // (hook activation follows IsFeatureActive()), so nothing needs to be
        // patched eagerly here.
        return true;
    }

    FailedWallRunPlugin g_failedWallRun;
} g_thisPlugin;
