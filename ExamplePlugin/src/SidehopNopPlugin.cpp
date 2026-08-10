#include "pch.h"
#include "SidehopNopPlugin.h"
#include <shlobj.h>
#include <fstream>

static constexpr uintptr_t STATIC_ADDR = 0x14015367f;
static constexpr uintptr_t EXPECTED_BASE = 0x140000000;
static constexpr int RVA = STATIC_ADDR - EXPECTED_BASE;
static constexpr uint8_t NOP_BYTES[4] = { 0x90, 0x90, 0x90, 0x90 };

SidehopNopPlugin::SidehopNopPlugin()
{
    HMODULE hMod = GetModuleHandleW(L"ACU.exe");
    if (!hMod)
        hMod = GetModuleHandleW(nullptr);
    m_TargetAddress = (uintptr_t)hMod + RVA;

    DWORD oldProtect = 0;
    if (VirtualProtect((LPVOID)m_TargetAddress, sizeof(m_OriginalBytes), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        memcpy(m_OriginalBytes.data(), (LPCVOID)m_TargetAddress, sizeof(m_OriginalBytes));
        m_HaveOriginalBytes = true;
        VirtualProtect((LPVOID)m_TargetAddress, sizeof(m_OriginalBytes), oldProtect, &oldProtect);
    }
}

SidehopNopPlugin::~SidehopNopPlugin()
{
    RestoreBytes();
}

std::string SidehopNopPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\SidehopNop.ini";
    return "SidehopNop.ini";
}

void SidehopNopPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("Enabled=", 0) == 0)
            m_Enabled = (line.substr(8) == "1");
        else if (line.rfind("KeyBind=", 0) == 0)
            try { m_KeyBind = std::stoi(line.substr(8)); } catch (...) {}
    }
}

void SidehopNopPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (f)
            f << "Enabled=" << (m_Enabled ? 1 : 0) << "\n"
              << "KeyBind=0x" << std::hex << m_KeyBind << std::dec << "\n";
    } catch (...) {}
}

void SidehopNopPlugin::ApplyNop()
{
    if (!m_HaveOriginalBytes || m_NopCurrentlyActive) return;
    DWORD oldProtect = 0;
    if (VirtualProtect((LPVOID)m_TargetAddress, sizeof(NOP_BYTES), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        memcpy((LPVOID)m_TargetAddress, NOP_BYTES, sizeof(NOP_BYTES));
        VirtualProtect((LPVOID)m_TargetAddress, sizeof(NOP_BYTES), oldProtect, &oldProtect);
        m_NopCurrentlyActive = true;
    }
}

void SidehopNopPlugin::RestoreBytes()
{
    if (!m_HaveOriginalBytes || !m_NopCurrentlyActive) return;
    DWORD oldProtect = 0;
    if (VirtualProtect((LPVOID)m_TargetAddress, sizeof(m_OriginalBytes), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        memcpy((LPVOID)m_TargetAddress, m_OriginalBytes.data(), sizeof(m_OriginalBytes));
        VirtualProtect((LPVOID)m_TargetAddress, sizeof(m_OriginalBytes), oldProtect, &oldProtect);
        m_NopCurrentlyActive = false;
    }
}

void SidehopNopPlugin::OnUpdate()
{
    bool keyHeld = (GetAsyncKeyState(m_KeyBind) & 0x8000) != 0;
    bool shouldNop = m_Enabled && keyHeld;

    if (shouldNop && !m_NopCurrentlyActive)
        ApplyNop();
    else if (!shouldNop && m_NopCurrentlyActive)
        RestoreBytes();
}

void SidehopNopPlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Sidehop NOP"))
        return;

    ImGui::TextWrapped("When enabled and the bind key is held, "
        "NOOPs 4 bytes at 0x14015367f to unlock an almost-functional sidehop-to-hang.");

    bool enabled = m_Enabled;
    if (ImGui::Checkbox("Enable Sidehop NOP", &enabled))
    {
        m_Enabled = enabled;
        if (!m_Enabled) RestoreBytes();
        SaveSettings();
    }

    if (m_Enabled)
    {
        ImGui::Indent();
        ImGui::Text("Bind key (hold): 0x%02X", m_KeyBind);
        ImGui::SameLine();
        if (ImGui::Button("Rebind##snh"))
        {
            for (int vk = 1; vk <= 0xFE; ++vk)
            {
                if (GetAsyncKeyState(vk) & 0x8000)
                {
                    m_KeyBind = vk;
                    SaveSettings();
                    break;
                }
            }
        }

        ImGui::Text("Status:");
        ImGui::SameLine();
        ImGui::TextColored(m_NopCurrentlyActive
            ? ImVec4(0.2f, 0.9f, 0.3f, 1.0f)
            : ImVec4(1.0f, 0.55f, 0.1f, 1.0f),
            m_NopCurrentlyActive ? "NOP ACTIVE" : "INACTIVE");
        ImGui::Unindent();
    }
}
