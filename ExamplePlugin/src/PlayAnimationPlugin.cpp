#include "pch.h"
#include "PlayAnimationPlugin.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/ManagedPtrs/ManagedPtrs.h"
#include "ACU/Animation.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>

DEFINE_GAME_FUNCTION(oneOfThoseFns_WhenStartActing, 0x141AC3CA0, __int64, __fastcall,
    (HumanStatesHolder* p_humanStates, char* a2, Animation* p_anim, uint32 a4, char a5, char a6));

static void PlayAnimationRaw(HumanStatesHolder* hs, Animation* anim)
{
    __try {
        char a2 = 0;
        oneOfThoseFns_WhenStartActing(hs, &a2, anim, 0, 1, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void PlayAnimationByHandle(uint64 handle)
{
    if (handle == 0) return;
    ACU::StrongRef<Animation> sharedAnim(handle);
    Animation* anim = sharedAnim.GetPtr();
    if (!anim) return;
    HumanStatesHolder* hs = HumanStatesHolder::GetForPlayer();
    if (!hs) return;
    PlayAnimationRaw(hs, anim);
}

static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\PlayAnimation.ini";
    return "PlayAnimation.ini";
}

void PlayAnimationPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("AnimHandle=", 0) == 0)
            try { m_AnimHandle = std::stoull(line.substr(11)); } catch (...) {}
    }
}

void PlayAnimationPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (f)
            f << "AnimHandle=" << m_AnimHandle << "\n";
    } catch (...) {}
}

void PlayAnimationPlugin::OnUpdate()
{
    // Toggle on F8
    bool keyDown = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    if (keyDown && !m_PrevKeyState)
    {
        m_Enabled = !m_Enabled;
        SaveSettings();
    }
    m_PrevKeyState = keyDown;

    if (!m_Enabled) return;

    // Press V to play animation
    if (GetAsyncKeyState(0x56) & 0x8000)
        PlayAnimationByHandle(m_AnimHandle);
}

void PlayAnimationPlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Play Animation"))
        return;

    ImGui::Text("Enabled: %s", m_Enabled ? "ON" : "OFF");
    ImGui::Text("Toggle key: F8");
    ImGui::Text("Play key: V");
    ImGui::Separator();

    static char buf[32] = "";
    ImGui::Text("Animation handle:");
    ImGui::PushItemWidth(180);
    if (ImGui::InputText("##animhandle", buf, sizeof(buf)))
    {
        try { m_AnimHandle = std::stoull(buf); SaveSettings(); }
        catch (...) {}
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        try { m_AnimHandle = std::stoull(buf); SaveSettings(); }
        catch (...) {}
    }
    if (m_AnimHandle != 0)
        ImGui::Text("Active handle: %llu", m_AnimHandle);
}
