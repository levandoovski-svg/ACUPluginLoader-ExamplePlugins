#include "pch.h"
#include "DodgeAnywherePlugin.h"
#include "MyLog.h"
#include <Windows.h>
#include "ACU/CLAssassin.h"   // for CLAssassin::GetSingleton()

// -----------------------------------------------------------
// Helpers
// -----------------------------------------------------------
static bool IsPlausiblePointer(uintptr_t p) {
    return p > 0x10000 && p < 0x0007FFFFFFFFFFFFull;
}

static uint8_t* GetAssassinTaskBase() {
    CLAssassin* cl = CLAssassin::GetSingleton();
    if (!cl || !IsPlausiblePointer((uintptr_t)cl)) return nullptr;
    // assassinTask is at offset 0xA8 (from CLAssassin.h)
    auto* taskPtr = *(uintptr_t*)((uintptr_t)cl + 0xA8);
    if (!IsPlausiblePointer(taskPtr)) return nullptr;
    return (uint8_t*)taskPtr;
}

// -----------------------------------------------------------
// Snapshot capture
// -----------------------------------------------------------
void DodgeAnywherePlugin::CaptureSnapshot(uint8_t* buffer) {
    uint8_t* base = GetAssassinTaskBase();
    if (!base) {
        memset(buffer, 0, 0x200);
        return;
    }
    memcpy(buffer, base, 0x200);
}

void DodgeAnywherePlugin::Compare() {
    m_ScanResults.clear();
    if (!m_CaptureOut || !m_CaptureIn) {
        m_ScanResults.push_back("Error: Both captures needed.");
        return;
    }
    int changes = 0;
    char line[128];
    for (size_t i = 0; i < 0x200; ++i) {
        if (m_SnapshotOut[i] != m_SnapshotIn[i]) {
            sprintf_s(line, "Offset 0x%04X: OUT=%02X IN=%02X", (unsigned)i, m_SnapshotOut[i], m_SnapshotIn[i]);
            m_ScanResults.push_back(line);
            changes++;
        }
    }
    sprintf_s(line, "Total changed bytes: %d", changes);
    m_ScanResults.push_back(line);
    m_CaptureOut = false;
    m_CaptureIn = false;
}

// -----------------------------------------------------------
// Main update (empty, scanner only)
// -----------------------------------------------------------
void DodgeAnywherePlugin::OnUpdate() {
    if (!m_Enabled) return;
    if (m_WaitingForKey) {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000) { m_Key = vk; m_WaitingForKey = false; break; }
        return;
    }
}

// -----------------------------------------------------------
// ImGui
// -----------------------------------------------------------
void DodgeAnywherePlugin::OnImGuiRender() {
    if (!ImGui::Begin("Dodge Anywhere")) { ImGui::End(); return; }
    ImGui::TextColored(m_Enabled ? ImVec4(0.2f,1,0.2f,1) : ImVec4(1,0.4f,0.4f,1),
                       m_Enabled ? "Enabled" : "Disabled");
    ImGui::Checkbox("Enable dodge anywhere", &m_Enabled);
    if (m_WaitingForKey) ImGui::TextColored({1,1,0,1}, "Press any key...");
    else {
        ImGui::Text("Key (unused): VK 0x%X", m_Key);
        if (ImGui::Button("Rebind")) m_WaitingForKey = true;
    }
    ImGui::Separator();
    ImGui::Text("AssassinTask Combat Flag Scanner");
    ImGui::Text("1. OUT of combat -> Capture OUT");
    if (ImGui::Button("Capture OUT")) {
        CaptureSnapshot(m_SnapshotOut);
        m_CaptureOut = true;
    }
    ImGui::SameLine(); ImGui::Text(m_CaptureOut ? "OK" : "not captured");
    ImGui::Text("2. IN combat -> Capture IN");
    if (ImGui::Button("Capture IN")) {
        CaptureSnapshot(m_SnapshotIn);
        m_CaptureIn = true;
    }
    ImGui::SameLine(); ImGui::Text(m_CaptureIn ? "OK" : "not captured");
    if (ImGui::Button("Compare & Show Results")) Compare();
    if (!m_ScanResults.empty()) {
        ImGui::BeginChild("Results", ImVec2(0, 200), true);
        for (const auto& s : m_ScanResults) ImGui::TextUnformatted(s.c_str());
        ImGui::EndChild();
    }
    ImGui::End();
}