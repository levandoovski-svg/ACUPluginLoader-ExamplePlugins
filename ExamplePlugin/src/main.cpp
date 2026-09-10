#include "pch.h"

#include "MyLog.h"
#include "Hack_ParkourDistanceOverride.h"

#include "Common_Plugins/Common_PluginSide.h"

#define LOG_FILENAME    THIS_DLL_PROJECT_NAME "-log.log"

std::optional<MyLogFileLifetime> g_LogLifetime;

class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        ImGui::Text("Hello from " THIS_DLL_PROJECT_TARGET_FILE_NAME " plugin!");
        ImGui::Separator();
        if (m_hackCurrent)
            m_hackCurrent->DrawImGuiControls();
    }
    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
    }
    virtual uint64 GetThisPluginVersion() override
    {
        // _Your_ plugin version. Currently is for logging only. (In the future, potentially for interplugin communications.)
        return MAKE_VERSION_NUMBER_UINT64(0, 0, 8, 0);
    }
    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
        // Will be called soon after loading if the PluginLoader determines your Plugin API version to be compatible.
        // It is okay to do stuff here that doesn't depend on the game, like initializing a log and reading a config file, for example.
        // It is not okay to try to patch the game's code, call the game's code, access the game's globals here.

        g_LogLifetime.emplace(AbsolutePathInThisDLLDirectory(LOG_FILENAME));
    }
    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        // This is probably where you want to initialize your plugin.
        // Will be called only if the plugin loader confirms that your plugin API version is compatible.
        // At this point the Main Integrity Check is killed, and the game's code can be safely patched.
        // Return `false` to unload the plugin.

        if (!m_hackCurrent)
            m_hackCurrent = std::make_unique<Hack_ParkourDistanceOverride>();
        m_hackCurrent->OnBeforeActivate();
        return true;
    }

    std::unique_ptr<Hack_ParkourDistanceOverride> m_hackCurrent;
    
    } g_thisPlugin;