#include "pch.h"
#include "AnimationOverridePlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include "ACU/AtomAnimComponent.h"
#include "ACU/AtomGraph.h"
#include "ACU/AtomGraphStateNode.h"
#include "ACU/AtomAnimationDataBaseNode.h"
#include "ACU/Animation.h"
#include "ACU/AtomStateMachineNode.h"
#include "ACU/ManagedPtrs/ManagedPtrs.h"
#include "ACU/ManagedPtrs/AllManagedObjects.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include "SharedStateEnterDispatcher.h"

static bool g_OverrideEnabled = false;
static uint64 g_OverrideHandle = 32322693439; // the specific assassinate anim

// Per-state counters
static int g_Count_PP = 0;
static int g_Count_P = 0;
static int g_Count_FirstHalf = 0;
static int g_Count_SecondHalf = 0;
static bool g_OverrideApplied = false;

// ── Get player's AtomGraph ─────────────────────────────────
static AtomGraph* GetPlayerAtomGraph()
{
    __try {
        Entity* player = ACU::GetPlayer();
        if (!player) return nullptr;
        int idx = player->cpntIndices_157.atomAnimCpnt;
        AtomAnimComponent* ac = (AtomAnimComponent*)player->cpnts_mb[idx];
        if (!ac) return nullptr;
        return ac->shared_AtomGraph_NewDemo_DEV->GetPtr();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ── Replace shared_Animation on all anim data nodes ────────
static void OverrideAnimNodesWithHandle(AtomGraph* graph, uint64 handle)
{
    if (!graph || !graph->RootStateMachine || handle == 0) return;
    __try {
        SharedBlock& block = FindOrMakeSharedBlockByHandleAndIncrementStrongRefcount(handle);
        auto& states = graph->RootStateMachine->States;
        for (uint32_t si = 0; si < states.size; si++)
        {
            AtomGraphStateNode* gs = (AtomGraphStateNode*)states.arr[si];
            if (!gs) continue;
            for (uint32_t ni = 0; ni < gs->Nodes.size; ni++)
            {
                AtomAnimationDataBaseNode* animNode =
                    (AtomAnimationDataBaseNode*)gs->Nodes.arr[ni];
                if (!animNode || !animNode->shared_Animation) continue;
                animNode->shared_Animation = &static_cast<SharedPtrNew<Animation>&>(block);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Hook at universal state enter dispatcher (0x1427555D2) ─
static void OnStateEnter(AllRegisters* params)
{
    if (!g_OverrideEnabled) return;

    uint64_t rax = params->GetRAX();

    if (rax == 0x141A4A4B0)
        g_Count_PP++;
    else if (rax == 0x141A462F0)
        g_Count_P++;
    else if (rax == 0x141A42F60)
    {
        g_Count_FirstHalf++;
        if (g_OverrideHandle != 0)
        {
            AtomGraph* graph = GetPlayerAtomGraph();
            if (graph) { OverrideAnimNodesWithHandle(graph, g_OverrideHandle); g_OverrideApplied = true; }
        }
    }
    else if (rax == 0x141A40BE0)
    {
        g_Count_SecondHalf++;
        if (g_OverrideHandle != 0)
        {
            AtomGraph* graph = GetPlayerAtomGraph();
            if (graph) { OverrideAnimNodesWithHandle(graph, g_OverrideHandle); g_OverrideApplied = true; }
        }
    }
}

static void InstallHooks()
{
    SharedStateEnterDispatcher::Subscribe(OnStateEnter);
}

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\AnimationOverride.ini";
    return "AnimationOverride.ini";
}

void AnimationOverridePlugin::LoadSettings()
{
    InstallHooks();

    std::ifstream f(GetIniPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("ToggleKey=", 0) == 0)
            try { m_ToggleKey = std::stoi(line.substr(10), nullptr, 16); } catch (...) {}
        if (line.rfind("Enabled=", 0) == 0)
            try { m_Enabled = std::stoi(line.substr(8)) != 0; } catch (...) {}
        if (line.rfind("OverrideHandle=", 0) == 0)
            try { g_OverrideHandle = std::stoull(line.substr(15)); } catch (...) {}
    }
    g_OverrideEnabled = m_Enabled;
}

void AnimationOverridePlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (f)
            f << "ToggleKey=" << std::hex << m_ToggleKey << "\n"
              << std::dec << "Enabled=" << (m_Enabled ? 1 : 0) << "\n"
              << "OverrideHandle=" << g_OverrideHandle << "\n";
    } catch (...) {}
}

void AnimationOverridePlugin::OnUpdate()
{
    if (m_WaitingForKey)
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000)
            {
                m_ToggleKey = vk; m_WaitingForKey = false;
                SaveSettings(); return;
            }
        return;
    }

    bool keyDown = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    if (keyDown && !m_PrevKeyState)
    {
        m_Enabled = !m_Enabled;
        g_OverrideEnabled = m_Enabled;
        SaveSettings();
    }
    m_PrevKeyState = keyDown;
}

void AnimationOverridePlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Animation Override"))
        return;
    ImGui::Text("Status: %s", m_Enabled ? "ON" : "OFF");
    ImGui::Separator();
    ImGui::Text("State counters:");
    ImGui::Text("  PP: %d  P: %d  FirstHalf: %d  SecondHalf: %d",
        g_Count_PP, g_Count_P, g_Count_FirstHalf, g_Count_SecondHalf);

    if (g_OverrideApplied)
        ImGui::TextColored({0,1,0,1}, "Override applied on last assassination");
    ImGui::Separator();

    ImGui::Text("Override animation handle:");
    static char buf[32] = "";
    ImGui::PushItemWidth(180);
    if (ImGui::InputText("##handle", buf, sizeof(buf)))
    {
        try { g_OverrideHandle = std::stoull(buf); SaveSettings(); }
        catch (...) {}
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Set"))
    {
        try { g_OverrideHandle = std::stoull(buf); SaveSettings(); }
        catch (...) {}
    }
    ImGui::SameLine();
    if (ImGui::Button("Default"))
    {
        g_OverrideHandle = 32322693439; buf[0] = '\0'; SaveSettings();
    }

    if (g_OverrideHandle != 0)
    {
        ImGui::Text("Active handle: %llu (0x%llX)", g_OverrideHandle, g_OverrideHandle);
        ImGui::TextWrapped("When enabled, replaces shared_Animation on all animation data nodes with the animation loaded from this handle during assassination.");
    }
    ImGui::Separator();
    ImGui::Text("Toggle key: 0x%02X", m_ToggleKey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey ? "Cancel" : "Rebind"))
        m_WaitingForKey = !m_WaitingForKey;
    if (m_WaitingForKey)
        ImGui::TextColored({1,1,0,1}, "Press any key...");
}
