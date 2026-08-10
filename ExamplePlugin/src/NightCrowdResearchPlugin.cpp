#include "pch.h"
#include "NightCrowdResearchPlugin.h"
#include <fstream>
#include <sstream>
#include <string>

std::wstring NightCrowdResearchPlugin::GetIniPath() const
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleW(L"ExamplePlugin"), path, MAX_PATH);
    std::wstring p(path);
    auto pos = p.rfind(L'\\');
    if (pos != std::wstring::npos)
        p.resize(pos + 1);
    return p + L"NightCrowd.ini";
}

void NightCrowdResearchPlugin::LoadConfig()
{
    m_Enabled = false;
    m_HidePercent = 80;

    std::wifstream f(GetIniPath());
    if (!f.is_open())
    {
        SaveConfig();
        return;
    }

    std::wstring line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == L';' || line[0] == L'#')
            continue;
        auto eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        if (key == L"Enabled")
            m_Enabled = (val == L"1");
        else if (key == L"HidePercent")
            m_HidePercent = std::stoi(val);
    }
}

void NightCrowdResearchPlugin::SaveConfig()
{
    std::wofstream f(GetIniPath());
    if (!f.is_open())
        return;
    f << L"Enabled=" << (m_Enabled ? L"1" : L"0") << L"\n";
    f << L"HidePercent=" << m_HidePercent << L"\n";
}

void NightCrowdResearchPlugin::Initialize()
{
    LoadConfig();
    m_ConfigLoaded = true;
}

void NightCrowdResearchPlugin::OnUpdate()
{
}

void NightCrowdResearchPlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Night Crowd Control"))
        return;

    ImGui::Text("Plugin loaded successfully.");
    ImGui::Text("Config loaded: %s", m_ConfigLoaded ? "yes" : "no");
    ImGui::Text("Enabled: %s", m_Enabled ? "yes" : "no");
    ImGui::Text("HidePercent: %d", m_HidePercent);

    if (ImGui::Button("Save Config"))
        SaveConfig();

    if (ImGui::Button("Reload Config"))
        LoadConfig();
}
