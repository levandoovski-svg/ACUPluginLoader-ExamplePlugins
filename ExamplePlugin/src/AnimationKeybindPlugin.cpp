#include "pch.h"
#include "AnimationKeybindPlugin.h"
#include "AnimationPlayer.h"
#include "Log.h"

#include <fstream>
#include <Windows.h>
#include <shlobj.h>
#include <sstream>

static std::string GetConfigPath()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        std::string path(documents);
        path += "\\Assassin's Creed Unity\\AnimationKeybindPlugin.ini";
        return path;
    }
    return "AnimationKeybindPlugin.ini";
}

bool AnimationKeybindPlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}

void AnimationKeybindPlugin::LoadSettings()
{
    std::ifstream file(GetConfigPath());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("NumSlots=", 0) == 0)
        {
            int count = std::stoi(line.substr(9));
            m_slots.resize(count);
        }
        else if (line.rfind("Slot=", 0) == 0)
        {
            std::string data = line.substr(5);
            std::replace(data.begin(), data.end(), ',', ' ');
            std::istringstream iss(data);
            int idx, key;
            uint64 handle;
            int enabled;
            std::string lbl;
            if (iss >> idx >> key >> handle >> enabled)
            {
                std::getline(iss, lbl);
                if (!lbl.empty() && lbl[0] == ' ') lbl.erase(0, 1);
                if (idx >= 0 && idx < (int)m_slots.size())
                {
                    m_slots[idx].keyCode = key;
                    m_slots[idx].animHandle = handle;
                    m_slots[idx].enable = enabled != 0;
                    m_slots[idx].label = lbl;
                    m_slots[idx].waitingForKey = false;
                }
            }
        }
    }
}

void AnimationKeybindPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetConfigPath());
        if (file)
        {
            file << "NumSlots=" << m_slots.size() << "\n";
            for (size_t i = 0; i < m_slots.size(); ++i)
            {
                auto& s = m_slots[i];
                file << "Slot=" << i << "," << s.keyCode << "," << s.animHandle << "," << (s.enable ? 1 : 0) << "," << s.label << "\n";
            }
        }
    } catch (...) {}
}

void AnimationKeybindPlugin::OnBeforeActivate()
{
    PluginLog("OnBeforeActivate: START");
    LoadSettings();
    if (m_slots.empty())
    {
        m_slots.emplace_back();
        m_slots.back().keyCode = VK_F9;
        m_slots.back().label = "Slot 1";
    }
    PluginLog("OnBeforeActivate: END");
}

void AnimationKeybindPlugin::OnBeforeDeactivate()
{
    PluginLog("OnBeforeDeactivate");
    SaveSettings();
}

void AnimationKeybindPlugin::OnUpdate()
{
    PluginLog("OnUpdate: START");

    g_MyAnimationPlayer.UpdateAnimations();
    PluginLog("OnUpdate: UpdateAnimations done");

    for (auto& slot : m_slots)
    {
        if (!slot.enable) continue;

        if (slot.waitingForKey)
        {
            for (int vk = 1; vk <= 0xFE; ++vk)
            {
                if (IsRisingEdgePressed(vk))
                {
                    slot.keyCode = vk;
                    slot.waitingForKey = false;
                    SaveSettings();
                    break;
                }
            }
            continue;
        }

        if (IsRisingEdgePressed(slot.keyCode))
        {
            PluginLog("OnUpdate: key pressed, about to check anim ptr");
            if (slot.anim.GetPtr())
            {
                PluginLog("OnUpdate: about to call StartAnimation");
                g_MyAnimationPlayer.StartAnimation(slot.anim);
                PluginLog("OnUpdate: StartAnimation returned OK");
            }
            else
            {
                PluginLog("OnUpdate: slot.anim is null, nothing to play");
            }
        }
    }
    PluginLog("OnUpdate: END");
}

void AnimationKeybindPlugin::PickAnimation(int slotIdx)
{
    if (slotIdx < 0 || slotIdx >= (int)m_slots.size()) return;
    auto& slot = m_slots[slotIdx];

    PluginLog("PickAnimation: about to call Draw");
    if (m_Picker.Draw("Pick Animation", slot.anim))
    {
        PluginLog("PickAnimation: picker returned true (new anim picked)");
        if (Animation* anim = slot.anim.GetPtr())
        {
            slot.animHandle = slot.anim.GetSharedBlock().handle;
            PluginLog("PickAnimation: saved anim handle");
        }
        else
        {
            slot.animHandle = 0;
            PluginLog("PickAnimation: anim is null after pick");
        }
        SaveSettings();
    }
    PluginLog("PickAnimation: END");
}

void AnimationKeybindPlugin::OnImGuiRender()
{
    ImGui::Text("Animation Keybind Plugin");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Animation Player Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool looping = g_MyAnimationPlayer.m_isLooping;
        if (ImGui::Checkbox("Looping", &looping))
        {
            g_MyAnimationPlayer.m_isLooping = looping;
        }
        float speed = g_MyAnimationPlayer.m_speedMult;
        if (ImGui::SliderFloat("Speed", &speed, -2.0f, 2.0f))
        {
            if (speed > -0.01f && speed < 0.01f) speed = 0.01f;
            g_MyAnimationPlayer.m_speedMult = speed;
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Keybind Slots", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("+ Add Slot"))
        {
            AnimationSlot newSlot;
            newSlot.keyCode = VK_F9 + (int)m_slots.size();
            newSlot.label = "Slot " + std::to_string(m_slots.size() + 1);
            m_slots.push_back(newSlot);
            SaveSettings();
        }

        if (m_slots.empty())
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No slots defined.");
        }
        else
        {
            const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
            const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
            const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);

            for (int idx = 0; idx < (int)m_slots.size(); ++idx)
            {
                auto& slot = m_slots[idx];
                ImGui::PushID(idx);

                bool hasAnim = slot.anim.GetPtr() != nullptr;
                ImGui::TextColored(hasAnim ? activeColor : inactiveColor,
                    hasAnim ? "[ANIM LOADED]" : "[NO ANIM]");
                ImGui::SameLine();

                if (ImGui::CollapsingHeader(slot.label.empty() ? "Unnamed" : slot.label.c_str()))
                {
                    ImGui::Text("Label:");
                    ImGui::SameLine();
                    char buf[128] = {};
                    strncpy_s(buf, slot.label.c_str(), sizeof(buf) - 1);
                    if (ImGui::InputText("##label", buf, sizeof(buf)))
                    {
                        slot.label = buf;
                        SaveSettings();
                    }

                    ImGui::Checkbox("Enabled", &slot.enable);

                    ImGui::Text("Key: 0x%02X (%d)", slot.keyCode, slot.keyCode);
                    ImGui::SameLine();
                    if (ImGui::Button(slot.waitingForKey ? "Cancel" : "Rebind"))
                        slot.waitingForKey = !slot.waitingForKey;
                    if (slot.waitingForKey)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(rebindColor, "Press any key...");
                    }

                    ImGui::Separator();
                    PickAnimation(idx);

                    if (slot.anim.GetPtr())
                    {
                        float duration = slot.anim.GetPtr()->Length;
                        ImGui::Text("Duration: %.3f sec", duration);
                        ImGui::Text("Handle: %llu", slot.animHandle);
                    }

                    ImGui::Separator();
                    if (ImGui::Button("Delete Slot"))
                    {
                        m_slots.erase(m_slots.begin() + idx);
                        SaveSettings();
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }
        }
    }
}
