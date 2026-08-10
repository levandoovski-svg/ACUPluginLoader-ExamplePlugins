
#include "pch.h"

#include "MyLog.h"
#include "MainConfig.h"
#include "DesyncKeybindPlugin.h"

#include "Common_Plugins/Common_PluginSide.h"

#define LOG_FILENAME    THIS_DLL_PROJECT_NAME "-log.log"
#define CONFIG_FILENAME THIS_DLL_PROJECT_NAME "-config.json"

std::optional<MyLogFileLifetime> g_LogLifetime;
class ExamplePlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual void EveryFrameWhenMenuIsOpen() override
    {
        // You can draw the contents of your ImGui menu here.

        ImGui::Text("Hello from " THIS_DLL_PROJECT_TARGET_FILE_NAME " plugin!");
        ImGui::Separator();
        m_DesyncKeybind.OnImGuiRender();
    }
    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        m_DesyncKeybind.OnUpdate();
    }
    virtual uint64 GetThisPluginVersion() override
    {
        // _Your_ plugin version. Currently is for logging only. (In the future, potentially for interplugin communications.)
        return MAKE_VERSION_NUMBER_UINT64(0, 0, 3, 0);
    }
    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
        // Will be called soon after loading if the PluginLoader determines your Plugin API version to be compatible.
        // It is okay to do stuff here that doesn't depend on the game, like initializing a log and reading a config file, for example.
        // It is not okay to try to patch the game's code, call the game's code, access the game's globals here.

        g_LogLifetime.emplace(AbsolutePathInThisDLLDirectory(LOG_FILENAME));
        MainConfig::FindAndLoadConfigFileOrCreateDefault(AbsolutePathInThisDLLDirectory(CONFIG_FILENAME));
        m_DesyncKeybind.LoadSettings();
    }
    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface& pluginLoader) override
    {
        // This is probably where you want to initialize your plugin.
        // Will be called only if the plugin loader confirms that your plugin API version is compatible.
        // At this point the Main Integrity Check is killed, and the game's code can be safely patched.
        // Return `false` to unload the plugin.

        LOG_DEBUG(DefaultLogger, "This line of text is written to both ImGui Console and the default log file\n");
        return true;
    }

    DesyncKeybindPlugin m_DesyncKeybind;
} g_thisPlugin;
