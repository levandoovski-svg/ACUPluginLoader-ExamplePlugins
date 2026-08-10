#include "pch.h"
#include "FreeJumpPlugin.h"
#include "DodgeAnywherePlugin.h"

static FreeJumpPlugin g_FreeJump;
static DodgeAnywherePlugin g_DodgeAnywhere;

static bool InitStage_WhenSafe(ACUPluginLoaderInterface& pluginLoader)
{
    g_FreeJump.OnBeforeActivate();
    return true;
}

static void EveryFrameEvenWhenMenuIsClosed(ImGuiShared& imGui)
{
    ImGui::SetCurrentContext(&imGui.m_ctx);
    g_FreeJump.OnUpdate();
    g_DodgeAnywhere.OnUpdate();
}

static void EveryFrameWhenMenuIsOpen(ImGuiShared& imGui)
{
    ImGui::SetCurrentContext(&imGui.m_ctx);
    g_FreeJump.OnImGuiRender();
    g_DodgeAnywhere.OnImGuiRender();
}

extern "C" __declspec(dllexport) bool ACUPluginStart(ACUPluginLoaderInterface& pluginLoader, ACUPluginInfo& yourPluginInfo_out)
{
    yourPluginInfo_out.m_PluginAPIVersion = g_CurrentPluginAPIversion;
    yourPluginInfo_out.m_PluginVersion = 0x00000100;
    yourPluginInfo_out.m_InitStage_WhenCodePatchesAreSafeToApply = InitStage_WhenSafe;
    yourPluginInfo_out.m_EveryFrameEvenWhenMenuIsClosed = EveryFrameEvenWhenMenuIsClosed;
    yourPluginInfo_out.m_EveryFrameWhenMenuIsOpen = EveryFrameWhenMenuIsOpen;
    return true;
}