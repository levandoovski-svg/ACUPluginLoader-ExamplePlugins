#include "pch.h"
#include "FreeJumpPlugin.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ACU/PlayerProgressionManager.h"
#include "ACU/AssassinAbilitySet.h"
#include <fstream>
#include <Windows.h>
#include <shlobj.h>
#include <cmath>
#include <sstream>

// GetEnumParkourAction definition (declared in AvailableParkourAction.h)
EnumParkourAction AvailableParkourAction::GetEnumParkourAction()
{
    return GET_AND_CAST_FANCY_FUNC(*this, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)(this);
}

// 1.5.0 address
using SmallArray_RemoveFunc = void(__fastcall*)(void* smallArray, int p_idx, unsigned int p_elemSize);
static SmallArray_RemoveFunc SmallArray_POD__RemoveGeneric = (SmallArray_RemoveFunc)0x142726000;

// ── AssassinAbilitySet helpers ────────────────────────────────────────

AssassinAbilitySet* FreeJumpPlugin::GetActiveAbilitySet()
{
    PlayerProgressionManager* mgr = PlayerProgressionManager::GetSingleton();
    if (!mgr) return nullptr;
    return mgr->assassinAbilitySets.GetHighestPrioritySet();
}

static std::string GetGameSaveFolder()
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documents)))
    {
        std::string path(documents);
        path += "\\Assassin's Creed Unity\\FreeJumpPlugin.ini";
        return path;
    }
    return "FreeJumpPlugin.ini";
}

void FreeJumpPlugin::LoadSettings()
{
    std::ifstream file(GetGameSaveFolder());
    if (!file) return;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.rfind("FreeJumpKey=", 0) == 0)
            try { m_ToggleKey = std::stoi(line.substr(13), nullptr, 16); } catch (...) {}
        else if (line.rfind("SnapDirectionKey=", 0) == 0)
            try { m_SnapDirectionKey = std::stoi(line.substr(17), nullptr, 16); } catch (...) {}
        else if (line.rfind("MouseLockEnabled=", 0) == 0)
            try { m_MouseLockEnabled = std::stoi(line.substr(17)) != 0; } catch (...) {}
        else if (line.rfind("NumOverrides=", 0) == 0)
        {
            int count = std::stoi(line.substr(13));
            m_Overrides.resize(count);
        }
        else if (line.rfind("Override=", 0) == 0)
        {
            std::string data = line.substr(9);
            std::replace(data.begin(), data.end(), ',', ' ');
            std::istringstream iss(data);
            int idx, key, removeOthers, enable, toggleMode, toggleState;
            float mult;
            std::string typeStr;
            if (iss >> idx >> key >> typeStr >> mult >> removeOthers >> enable >> toggleMode >> toggleState)
            {
                if (idx >= 0 && idx < (int)m_Overrides.size())
                {
                    m_Overrides[idx].keyCode = key;
                    m_Overrides[idx].actionTypes.clear();
                    std::istringstream typeStream(typeStr);
                    std::string seg;
                    while (std::getline(typeStream, seg, '|'))
                        if (!seg.empty())
                            m_Overrides[idx].actionTypes.push_back(std::stoi(seg));
                    if (m_Overrides[idx].actionTypes.empty())
                        m_Overrides[idx].actionTypes.push_back(50);
                    m_Overrides[idx].fitnessMultiplier = mult;
                    m_Overrides[idx].removeOtherActions = removeOthers != 0;
                    m_Overrides[idx].enable = enable != 0;
                    m_Overrides[idx].toggleMode = toggleMode != 0;
                    m_Overrides[idx].toggleState = toggleState != 0;
                }
            }
        }
    }
}

void FreeJumpPlugin::SaveSettings()
{
    try {
        std::ofstream file(GetGameSaveFolder());
        if (file)
        {
            file << "FreeJumpKey=" << std::hex << m_ToggleKey << std::dec << "\n";
            file << "SnapDirectionKey=" << std::hex << m_SnapDirectionKey << std::dec << "\n";
            file << "MouseLockEnabled=" << (m_MouseLockEnabled ? 1 : 0) << "\n";
            file << "NumOverrides=" << m_Overrides.size() << "\n";
            for (size_t i = 0; i < m_Overrides.size(); ++i)
            {
                auto& ov = m_Overrides[i];
                file << "Override=" << i << "," << ov.keyCode << ",";
                for (size_t t = 0; t < ov.actionTypes.size(); ++t)
                {
                    if (t > 0) file << "|";
                    file << ov.actionTypes[t];
                }
                file << "," << ov.fitnessMultiplier << "," << (ov.removeOtherActions ? 1 : 0) << ","
                     << (ov.enable ? 1 : 0) << "," << (ov.toggleMode ? 1 : 0) << ","
                     << (ov.toggleState ? 1 : 0) << "\n";
            }
        }
    } catch (...) {}
}

void FreeJumpPlugin::SaveCurrentFlags()
{
    AssassinAbilitySet* set = GetActiveAbilitySet();
    if (!set) return;
    m_SavedFlags.byte22 = *(uint8_t*)((uintptr_t)set + 0x22);
    m_SavedFlags.byte29 = *(uint8_t*)((uintptr_t)set + 0x29);
    m_SavedFlags.byte2A = *(uint8_t*)((uintptr_t)set + 0x2A);
    m_FlagsSaved = true;
}

void FreeJumpPlugin::RestoreFlags()
{
    if (!m_FlagsSaved) return;
    AssassinAbilitySet* set = GetActiveAbilitySet();
    if (!set) return;
    *(uint8_t*)((uintptr_t)set + 0x22) = m_SavedFlags.byte22;
    *(uint8_t*)((uintptr_t)set + 0x29) = m_SavedFlags.byte29;
    *(uint8_t*)((uintptr_t)set + 0x2A) = m_SavedFlags.byte2A;
    m_FlagsSaved = false;
}

void FreeJumpPlugin::ApplyMouseLock(bool enable)
{
    AssassinAbilitySet* set = GetActiveAbilitySet();
    if (!set) return;
    if (enable)
    {
        if (!m_FlagsSaved) SaveCurrentFlags();
        uint8_t* byte22 = (uint8_t*)((uintptr_t)set + 0x22);
        *byte22 &= ~(1 << 2);
        uint8_t* byte29 = (uint8_t*)((uintptr_t)set + 0x29);
        *byte29 &= ~((1 << 1) | (1 << 4));
        uint8_t* byte2A = (uint8_t*)((uintptr_t)set + 0x2A);
        *byte2A &= ~(1 << 0);
    }
    else RestoreFlags();
}

static Vector2f GetCameraFlatForward()
{
    auto* cam = ACUPlayerCameraComponent::GetSingleton();
    if (!cam) return { 0.0f, 1.0f };
    float x = *(float*)((uintptr_t)cam + 0x60);
    float y = *(float*)((uintptr_t)cam + 0x64);
    float z = *(float*)((uintptr_t)cam + 0x68);
    float w = *(float*)((uintptr_t)cam + 0x6C);
    float fx = 2.0f * (x*z + w*y);
    float fy = 2.0f * (y*z - w*x);
    return { fx, fy };
}

static void Rotate2D(float& x, float& y, float angle)
{
    float cosA = cosf(angle);
    float sinA = sinf(angle);
    float nx = x * cosA - y * sinA;
    float ny = x * sinA + y * cosA;
    x = nx; y = ny;
}

void FreeJumpPlugin::OnBeforeActivate()
{
    LoadSettings();
    if (m_MouseLockEnabled) ApplyMouseLock(true);

    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction*
    {
        return static_cast<FreeJumpPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.ChooseAfterSorting_fnp = nullptr;
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 5.0f;
    m_Callbacks.m_Name = "FreeJumpPlugin";
}

void FreeJumpPlugin::OnBeforeDeactivate()
{
    if (m_MouseLockEnabled) ApplyMouseLock(false);
    if (m_HookActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActive = false;
    }
    SaveSettings();
}

void FreeJumpPlugin::OnUpdate()
{
    // Rebind Free Jump key
    if (m_WaitingForKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (IsRisingEdgePressed(vk))
            {
                m_ToggleKey = vk;
                m_WaitingForKey = false;
                SaveSettings();
                break;
            }
        }
    }

    // Rebind Snap Direction key
    if (m_WaitingForSnapKey)
    {
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (IsRisingEdgePressed(vk))
            {
                m_SnapDirectionKey = vk;
                m_WaitingForSnapKey = false;
                break;
            }
        }
    }

    // Rebind custom overrides (they have waitingForKey)
    auto captureOverrideKey = [this](auto& ov, const char* name) -> bool
    {
        if (!ov.waitingForKey) return false;
        for (int vk = 1; vk <= 0xFE; ++vk)
        {
            if (IsRisingEdgePressed(vk))
            {
                ov.keyCode = vk;
                ov.waitingForKey = false;
                SaveSettings();
                return true;
            }
        }
        return false;
    };

    for (auto& ov : m_Overrides)
        if (captureOverrideKey(ov, "Custom override")) break;

    // Toggle snap direction
    static bool prevSnapKeyState = false;
    bool currSnapKeyState = (GetAsyncKeyState(m_SnapDirectionKey) & 1) != 0;
    if (currSnapKeyState && !prevSnapKeyState) m_SnapDirectionMode = !m_SnapDirectionMode;
    prevSnapKeyState = currSnapKeyState;

    // Update toggle state for overrides
    for (auto& ov : m_Overrides)
    {
        if (!ov.enable) continue;
        if (ov.toggleMode && IsRisingEdgePressed(ov.keyCode))
        {
            ov.toggleState = !ov.toggleState;
            SaveSettings();
        }
    }

    // Apply mouse lock state if changed
    static bool lastMouseLock = false;
    if (m_MouseLockEnabled != lastMouseLock)
    {
        ApplyMouseLock(m_MouseLockEnabled);
        lastMouseLock = m_MouseLockEnabled;
    }

    // Determine if hook should be active (via GenericHooksInParkourFiltering)
    bool freeJumpHeld = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    bool anyOverrideActive = false;
    for (auto& ov : m_Overrides)
        if (ov.enable && (ov.toggleMode ? ov.toggleState : (GetAsyncKeyState(ov.keyCode) & 0x8000)))
            anyOverrideActive = true;
    bool needHook = freeJumpHeld || anyOverrideActive || m_SnapDirectionMode;

    if (needHook && !m_HookActive)
    {
        auto gph = GenericHooksInParkourFiltering::GetSingleton();
        gph->Subscribe(m_Callbacks);
        m_HookActivator = gph->RequestGPHSortAndSelect();
        m_HookActive = true;
    }
    else if (!needHook && m_HookActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActive = false;
    }
}

void FreeJumpPlugin::OnImGuiRender()
{
    bool freeMode   = (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    bool snapMode   = m_SnapDirectionMode;
    const ImVec4 activeColor(0.20f, 0.90f, 0.30f, 1.00f);
    const ImVec4 inactiveColor(1.00f, 0.55f, 0.10f, 1.00f);
    const ImVec4 holdColor(0.20f, 0.70f, 1.00f, 1.00f);
    const ImVec4 rebindColor(0.90f, 0.90f, 0.20f, 1.00f);

    ImGui::Text("FreeJumpPlugin");
    if (ImGui::CollapsingHeader("Basic Features", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Free Jump:");
        ImGui::SameLine();
        ImGui::TextColored(freeMode ? activeColor : inactiveColor, freeMode ? "ACTIVE (HOLD)" : "OFF");
        ImGui::Text("  Bound key: 0x%02X", m_ToggleKey);
        if (m_WaitingForKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Press any key..."); }
        if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind##FreeJump"))
            m_WaitingForKey = !m_WaitingForKey;

        ImGui::Separator();
        ImGui::Text("Snap Direction:");
        ImGui::SameLine();
        ImGui::TextColored(snapMode ? ImVec4(0.20f,0.60f,0.90f,1.0f) : ImVec4(0.70f,0.70f,0.70f,1.0f),
                           snapMode ? "ON" : "OFF");
        ImGui::Text("  Toggle key: 0x%02X", m_SnapDirectionKey);
        if (m_WaitingForSnapKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Press any key..."); }
        if (ImGui::Button(m_WaitingForSnapKey ? "Cancel Rebind" : "Rebind##Snap"))
            m_WaitingForSnapKey = !m_WaitingForSnapKey;

        ImGui::Separator();
        bool mouseLock = m_MouseLockEnabled;
        if (ImGui::Checkbox("Disable Left/Right Mouse Buttons (sword/aim)", &mouseLock))
        { m_MouseLockEnabled = mouseLock; SaveSettings(); }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Parkour Action Overrides", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("+ Add Custom Override"))
        {
            ParkourActionOverride newOverride;
            newOverride.keyCode = VK_F9 + (int)m_Overrides.size();
            m_Overrides.push_back(newOverride);
            SaveSettings();
        }
        if (m_Overrides.empty())
            ImGui::TextColored(inactiveColor, "No custom overrides defined.");
        else
        {
            for (int idx = 0; idx < (int)m_Overrides.size(); ++idx)
            {
                auto& ov = m_Overrides[idx];
                ImGui::PushID(idx);
                bool active = ov.enable && (ov.toggleMode ? ov.toggleState : (GetAsyncKeyState(ov.keyCode) & 0x8000) != 0);
                ImGui::TextColored(active ? holdColor : inactiveColor, "[%s]",
                                   active ? "ACTIVE" : (ov.enable ? "ENABLED" : "DISABLED"));
                ImGui::SameLine();
                if (ImGui::CollapsingHeader(("Custom " + std::to_string(idx)).c_str()))
                {
                    ImGui::Checkbox("Enable", &ov.enable);
                    ImGui::SameLine();
                    ImGui::Text("Key: 0x%02X", ov.keyCode);
                    ImGui::SameLine();
                    if (ImGui::Button(ov.waitingForKey ? "Cancel" : "Rebind"))
                        ov.waitingForKey = !ov.waitingForKey;
                    if (ov.waitingForKey) ImGui::TextColored(rebindColor, "Press any key...");

                    bool holdMode = !ov.toggleMode;
                    if (ImGui::RadioButton("Hold (press and hold)", holdMode)) ov.toggleMode = false;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Toggle (press once to lock)", ov.toggleMode)) ov.toggleMode = true;

                    {
                        const char* kActionNames[] = {
                            "wallEjectToHang (17)",
                            "breakfall (18)",
                            "fromHangToWall_side (26)",
                            "hangToDropDown_safe (31)",
                            "riseFromHang (32)",
                            "jumpAndGrabFrontWall (35)",
                            "offTheWall_fromWallToGroundReleaseOrEject (36)",
                            "climbFacade_fromWallDescentSidewaysToGround_alsoSidehop (39)",
                            "dive (40)",
                            "swing_2F_SwingToSidewall_includingSpindescent (47)",
                            "swing_30_fromSwingToFeet (48)",
                            "swing_31_swingToSwing_mb (49)",
                            "offTheWall_fromWallToWallEjectAndCatch (50)",
                            "wallrunUpFromGroundFailed_mb (52)",
                            "wallrunUpFromGround_mb (53)",
                            "wallrunUpToHang (54)",
                            "onWallNormalClimb (65)",
                            "plainDismountFromWall_mb (69)",
                            "fromWallToHang_down (70)",
                            "fromWallToHang_side (71)",
                            "fromWallingToHangDownAndTurnCornerOutside (72)",
                            "fromWallToHang_up (77)",
                            "fromHangToWall_down (78)",
                            "backEject_4F (79)",
                        };
                        int kActionValues[] = { 17, 18, 26, 31, 32, 35, 36, 39, 40, 47, 48, 49, 50, 52, 53, 54, 65, 69, 70, 71, 72, 77, 78, 79 };
                        ImGui::Text("Action Types:");
                        for (int t = 0; t < (int)ov.actionTypes.size(); ++t)
                        {
                            ImGui::PushID(t);
                            if (ImGui::InputInt("##Type", &ov.actionTypes[t]))
                                SaveSettings();
                            ImGui::SameLine();
                            int presetIdx = -1;
                            for (int pi = 0; pi < IM_ARRAYSIZE(kActionValues); ++pi)
                                if (kActionValues[pi] == ov.actionTypes[t]) { presetIdx = pi; break; }
                            if (ImGui::Combo("##Preset", &presetIdx, kActionNames, IM_ARRAYSIZE(kActionNames)))
                            {
                                if (presetIdx >= 0 && presetIdx < IM_ARRAYSIZE(kActionValues))
                                    ov.actionTypes[t] = kActionValues[presetIdx];
                                SaveSettings();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("X"))
                            {
                                ov.actionTypes.erase(ov.actionTypes.begin() + t);
                                SaveSettings();
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::Button("+ Add Action Type"))
                        {
                            ov.actionTypes.push_back(50);
                            SaveSettings();
                        }
                    }
                    ImGui::DragFloat("Fitness Multiplier", &ov.fitnessMultiplier, 0.5f, 0.0f, 100000.0f);
                    ImGui::Checkbox("Remove all other actions", &ov.removeOtherActions);
                    if (ImGui::Button("Delete Override"))
                    {
                        m_Overrides.erase(m_Overrides.begin() + idx);
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

AvailableParkourAction* FreeJumpPlugin::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    // Free Jump
    if (GetAsyncKeyState(m_ToggleKey) & 0x8000)
    {
        actions.size = 0;
        return nullptr;
    }

    // Snap Direction
    if (m_SnapDirectionMode)
    {
        Vector2f camForward = GetCameraFlatForward();
        float camYaw = atan2f(camForward.x, camForward.y);
        float inputX = 0.0f, inputY = 0.0f;
        if (GetAsyncKeyState(0x57) & 0x8000) inputY += 1.0f;
        if (GetAsyncKeyState(0x53) & 0x8000) inputY -= 1.0f;
        if (GetAsyncKeyState(0x41) & 0x8000) inputX -= 1.0f;
        if (GetAsyncKeyState(0x44) & 0x8000) inputX += 1.0f;
        Vector2f inputDir(inputX, inputY);
        float len = sqrtf(inputDir.x * inputDir.x + inputDir.y * inputDir.y);
        if (len > 0.001f)
        {
            inputDir.x /= len;
            inputDir.y /= len;
            Rotate2D(inputDir.x, inputDir.y, camYaw);
            for (int i = 0; i < actions.size; ++i)
            {
                AvailableParkourAction* action = actions[i];
                if (!action) continue;
                Vector4f* dirPtr = (Vector4f*)((uintptr_t)action + 0x70);
                float dx = dirPtr->x, dy = dirPtr->y;
                float dLen = sqrtf(dx*dx + dy*dy);
                if (dLen < 0.001f) continue;
                float dot = (dx / dLen) * inputDir.x + (dy / dLen) * inputDir.y;
                if (dot < 0.0f)
                    *(float*)((uintptr_t)action + 0x204) = 0.0f;
            }
        }
    }

    // Custom overrides — collect all active override entries
    struct ActiveOverride { const ParkourActionOverride* ptr; };
    std::vector<ActiveOverride> actives;
    for (auto& ov : m_Overrides)
    {
        if (!ov.enable) continue;
        bool active = ov.toggleMode ? ov.toggleState : (GetAsyncKeyState(ov.keyCode) & 0x8000) != 0;
        if (!active) continue;
        actives.push_back({&ov});
    }

    if (!actives.empty())
    {
        // Build union of allowed action types across all active overrides
        std::vector<int> allowedTypes;
        bool shouldRemoveOthers = false;
        for (auto& ao : actives)
        {
            for (int type : ao.ptr->actionTypes)
                if (std::find(allowedTypes.begin(), allowedTypes.end(), type) == allowedTypes.end())
                    allowedTypes.push_back(type);
            if (ao.ptr->removeOtherActions)
                shouldRemoveOthers = true;
        }

        // Remove actions whose type is not in the allowed set
        if (shouldRemoveOthers)
        {
            for (int i = actions.size - 1; i >= 0; --i)
            {
                AvailableParkourAction* action = actions[i];
                if (!action) continue;
                if (std::find(allowedTypes.begin(), allowedTypes.end(),
                    (int)action->GetEnumParkourAction()) == allowedTypes.end())
                    SmallArray_POD__RemoveGeneric(&actions, i, sizeof(AvailableParkourAction*));
            }
        }

        // Apply fitness multipliers for matching actions
        for (auto& ao : actives)
        {
            for (int i = 0; i < actions.size; ++i)
            {
                AvailableParkourAction* action = actions[i];
                if (!action) continue;
                if (std::find(ao.ptr->actionTypes.begin(), ao.ptr->actionTypes.end(),
                    (int)action->GetEnumParkourAction()) != ao.ptr->actionTypes.end())
                {
                    float* fitness = (float*)((uintptr_t)action + 0x204);
                    if (ao.ptr->fitnessMultiplier != 1.0f)
                        *fitness *= ao.ptr->fitnessMultiplier;
                }
            }
        }
    }
    return nullptr;
}

bool FreeJumpPlugin::IsRisingEdgePressed(int vkCode)
{
    return (GetAsyncKeyState(vkCode) & 1) != 0;
}
