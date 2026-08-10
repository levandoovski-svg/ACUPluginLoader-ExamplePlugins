#pragma once

#include <cstdio>
#include <Windows.h>

inline void PluginLog(const char* msg)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "AnimationKeybindPlugin.log", "a") == 0 && f)
    {
        char buf[256];
        sprintf_s(buf, "[%u] %s\n", GetCurrentThreadId(), msg);
        fputs(buf, f);
        fflush(f);
        fclose(f);
    }
}
