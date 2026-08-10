#include "pch.h"
#include "FreeJumpPlugin.h"

static FreeJumpPlugin g_FreeJump;

static bool InitStage_WhenSafe(ACUPluginLoaderInterface& pluginLoader)
{
    g_FreeJump.OnBeforeActivate();
    return true;
}

static void EveryFrameEvenWhenMenuIsClosed(ImGuiShared& imGui)
{
    ImGui::SetCurrentContext(&imGui.m_ctx);
    g_FreeJump.OnUpdate();
}

static void EveryFrameWhenMenuIsOpen(ImGuiShared& imGui)
{
    ImGui::SetCurrentContext(&imGui.m_ctx);
    g_FreeJump.OnImGuiRender();
}

extern "C" __declspec(dllexport) bool ACUPluginStart(ACUPluginLoaderInterface& pluginLoader, ACUPluginInfo& yourPluginInfo_out)
{
    yourPluginInfo_out.m_PluginAPIVersion = g_CurrentPluginAPIversion;
    yourPluginInfo_out.m_PluginVersion    = 0x00000100;
    yourPluginInfo_out.m_InitStage_WhenCodePatchesAreSafeToApply = InitStage_WhenSafe;
    yourPluginInfo_out.m_EveryFrameEvenWhenMenuIsClosed          = EveryFrameEvenWhenMenuIsClosed;
    yourPluginInfo_out.m_EveryFrameWhenMenuIsOpen                = EveryFrameWhenMenuIsOpen;
    return true;
}