#pragma once

#include <filesystem>
namespace fs = std::filesystem;

#define LOG_DEBUG(LoggerVariable, fmt, ...) (LoggerVariable).LogDebug(fmt, ##__VA_ARGS__)

#define DEFINE_LOGGER_CONSOLE_AND_FILE(VariableName, textualName) Logger_ConsoleAndFile VariableName(textualName);
#define DEFINE_LOGGER_CONSOLE(VariableName, textualName) Logger_Console VariableName(textualName);
#define DEFINE_LOGGER_FILE(VariableName, textualName) Logger_File VariableName(textualName);
#define DEFINE_LOGGER_NULL(VariableName, textualName) Logger_Null VariableName;

extern FILE* g_LogFile;

class MyLogFileLifetime
{
public:
    MyLogFileLifetime(const fs::path& filepath);
    ~MyLogFileLifetime();
private:
    const fs::path& m_filepath;
};

struct Logger_Null
{
    static void LogDebug(const char* fmt, ...) {}
};
struct Logger_ConsoleAndFile
{
    const std::string m_Name;
    Logger_ConsoleAndFile(std::string_view name) : m_Name(name) {}
    void LogDebug(const char* fmt, ...)
    {
        if (!g_LogFile) return;
        va_list args;
        va_start(args, fmt);
        vfprintf(g_LogFile, fmt, args);
        va_end(args);
        fflush(g_LogFile);
    }
};
struct Logger_Console
{
    const std::string m_Name;
    Logger_Console(std::string_view name) : m_Name(name) {}
    static void LogDebug(const char* fmt, ...) {}
};
struct Logger_File
{
    const std::string m_Name;
    Logger_File(std::string_view name) : m_Name(name) {}
    void LogDebug(const char* fmt, ...)
    {
        if (!g_LogFile) return;
        va_list args;
        va_start(args, fmt);
        vfprintf(g_LogFile, fmt, args);
        va_end(args);
        fflush(g_LogFile);
    }
};

inline DEFINE_LOGGER_FILE(DefaultLogger, "[" THIS_DLL_PROJECT_TARGET_FILE_NAME "]");