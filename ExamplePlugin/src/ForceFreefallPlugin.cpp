#include "pch.h"
#include "ForceFreefallPlugin.h"
#include "SharedStateEnterDispatcher.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU/HumanStatesHolder.h"
#include <shlobj.h>
#include <fstream>

// ── Addresses (ACU 1.5.0, verified at runtime by previous sessions) ──────────
// UsingLift_Falling::Enter  — the freefall state we redirect to
static constexpr uint64_t ADDR_USINGLIFT_FALLING_ENTER = 0x141A3D2D0;

// ── Global redirect state (shared between OnUpdate and the enter hook) ───────
static bool g_WantRedirect = false;

// ── Hook callback: runs inside SharedStateEnterDispatcher on every Enter call ─
static void OnStateEnter(AllRegisters* params)
{
    if (!g_WantRedirect) return;

    // rcx = FunctorBase* node being entered
    FunctorBase* node = (FunctorBase*)params->rcx_;
    if (!node) return;

    // Basic sanity: parentStack should have at least one entry for any real state
    if (node->parentStack.size < 1) return;

    // Redirect: overwrite RAX (the Enter function about to be called)
    params->GetRAX() = ADDR_USINGLIFT_FALLING_ENTER;
    g_WantRedirect = false;
}

// ── INI path ─────────────────────────────────────────────────────────────────
static std::string GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\ForceFreefall.ini";
    return "ForceFreefall.ini";
}

// ── Lifecycle ────────────────────────────────────────────────────────────────
void ForceFreefallPlugin::Init()
{
    SharedStateEnterDispatcher::Initialize();
    SharedStateEnterDispatcher::Subscribe(OnStateEnter);
}

// ── Settings ─────────────────────────────────────────────────────────────────
void ForceFreefallPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("ActivateKey=", 0) == 0)
            try { m_ActivateKey = std::stoi(line.substr(12), nullptr, 16); } catch (...) {}
    }
}

void ForceFreefallPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (f)
            f << "ActivateKey=" << std::hex << m_ActivateKey << std::dec << "\n";
    } catch (...) {}
}

// ── Per-frame update ─────────────────────────────────────────────────────────
void ForceFreefallPlugin::OnUpdate()
{
    // Rebind key
    if (m_WaitingForKey)
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (GetAsyncKeyState(vk) & 1)
            {
                m_ActivateKey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                return;
            }
        return;
    }

    // Rising-edge detection
    static bool prevKeyState = false;
    bool keyDown = (GetAsyncKeyState(m_ActivateKey) & 0x8000) != 0;
    if (keyDown && !prevKeyState)
    {
        g_WantRedirect = true;
    }
    prevKeyState = keyDown;
}

// ── ImGui UI ─────────────────────────────────────────────────────────────────
void ForceFreefallPlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Force Freefall"))
        return;

    ImGui::TextWrapped("Press the bound key to force Arno into the freefall "
                       "(UsingLift_Falling) state on the next state transition.");

    ImGui::Separator();

    bool active = g_WantRedirect;
    ImGui::Text("Status:");
    ImGui::SameLine();
    ImGui::TextColored(active ? ImVec4(0.2f, 0.9f, 0.3f, 1.0f)
                              : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       active ? "REDIRECT PENDING (press key was hit)"
                              : "Standing by");

    ImGui::Text("Activate Key: 0x%02X", m_ActivateKey);
    ImGui::SameLine();
    if (ImGui::Button(m_WaitingForKey ? "Cancel##ff" : "Rebind##ff"))
        m_WaitingForKey = !m_WaitingForKey;
    if (m_WaitingForKey)
    {
        ImGui::SameLine();
        ImGui::TextColored({1,1,0,1}, "Press any key...");
    }
}
