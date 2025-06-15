# Example Plugin for ACUFixes' Plugin Loader

## If you have a DLL and you just want to adapt it for the Plugin Loader
Just do all of your initialization in `main.cpp->ExamplePlugin::InitStage_WhenCodePatchesAreSafeToApply()`.
When this method gets called, the Main Integrity Check is already disabled, and the game's code can be safely patched.
Return `false` to unload the plugin.

#### MainConfig(.h/.cpp)
This is an example of a config I use.
A bit clunky but can support booleans, numbers, enums.
You can remove these files and use any kind of config you want.

#### MyLog(.h/.cpp)
This is an example of a basic logger I use.
There are no logging levels, you just do
```c++
LOG_DEBUG(DefaultLogger, "This line of text is written to both ImGui Console and the default log file.\n");
```
Which will write the following line to both ImGui Console and the default log file:
```
[ExamplePlugin.dll]This line of text is written to both ImGui Console and the log file.
```
Example with some typical additional loggers:
```c++
DEFINE_LOGGER_CONSOLE_AND_FILE(Log_ConsoleAndFile, DefaultLogger.m_Name + "[ConsoleAndFile]");
DEFINE_LOGGER_CONSOLE(         Log_ConsoleOnly,    DefaultLogger.m_Name + "[ConsoleOnly]");

LOG_DEBUG(DefaultLogger, "Line 1\n");
LOG_DEBUG(Log_ConsoleOnly, "Line 2\n");
LOG_DEBUG(Log_ConsoleAndFile, "Line 3\n");
```
This results in output to default log file:
```
[ExamplePlugin.dll]Line1
[ExamplePlugin.dll][ConsoleAndFile]Line3
```
and output to console:
```
[ExamplePlugin.dll]Line1
[ExamplePlugin.dll][ConsoleOnly]Line2
[ExamplePlugin.dll][ConsoleAndFile]Line3
```
You can remove this basic logger and use any kind of log you want. In that case, for writing to ImGui Console, format the text yourself and use:
```c++
#include "Common_Plugins/Console/ImGuiConsole.h"
//...

ImGuiConsole::AddLog("Some text that only goes into ImGui Console");
```
