#include "pch.h"
#include "FreeJumpPlugin.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/EnumParkourAction.h"
#include "ParkourDebugging/FancyVFunctionDescription.h"
#include "ACU/PlayerProgressionManager.h"
#include "ACU/AssassinAbilitySet.h"
#include "ACU_DefineNativeFunction.h"
#include <Windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

// NOTE: AvailableParkourAction::GetEnumParkourAction() is defined exactly once,
// in GenericHooksInParkourFiltering.cpp (compiled in this project). Do NOT
// redefine it here — duplicate definitions break the link.

// ACU 1.5.0 / ACUFixes-0.9.3 verified SmallArray POD-removal entry point.
DEFINE_GAME_FUNCTION(SmallArray_POD__RemoveGeneric, 0x142725F00, void, __fastcall, (void*, int, unsigned int));

namespace
{
    constexpr int kMaxOverrideCount = 4096;

    std::string_view Trim(std::string_view text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        return text;
    }

    bool TryGetValue(std::string_view line, std::string_view prefix, std::string_view& value)
    {
        if (line.rfind(prefix, 0) != 0)
            return false;
        value = Trim(line.substr(prefix.size()));
        return true;
    }

    bool TryParseInteger(std::string_view text, int base, int& value)
    {
        text = Trim(text);
        if (text.empty())
            return false;

        try
        {
            size_t parsedCharacters = 0;
            const long long parsed = std::stoll(std::string(text), &parsedCharacters, base);
            if (parsedCharacters != text.size()
                || parsed < std::numeric_limits<int>::min()
                || parsed > std::numeric_limits<int>::max())
            {
                return false;
            }
            value = static_cast<int>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TryParseFloat(std::string_view text, float& value)
    {
        text = Trim(text);
        if (text.empty())
            return false;

        try
        {
            size_t parsedCharacters = 0;
            const float parsed = std::stof(std::string(text), &parsedCharacters);
            if (parsedCharacters != text.size() || !std::isfinite(parsed))
                return false;
            value = parsed;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TryParseBool(std::string_view text, bool& value)
    {
        text = Trim(text);
        if (text == "0")
        {
            value = false;
            return true;
        }
        if (text == "1")
        {
            value = true;
            return true;
        }
        return false;
    }

    bool IsValidVirtualKey(int virtualKey)
    {
        return virtualKey >= 1 && virtualKey <= 0xFE;
    }

    std::vector<std::string_view> Split(std::string_view text, char delimiter)
    {
        std::vector<std::string_view> parts;
        size_t partStart = 0;
        while (true)
        {
            const size_t delimiterPosition = text.find(delimiter, partStart);
            if (delimiterPosition == std::string_view::npos)
            {
                parts.push_back(text.substr(partStart));
                break;
            }
            parts.push_back(text.substr(partStart, delimiterPosition - partStart));
            partStart = delimiterPosition + 1;
        }
        return parts;
    }

    bool TryParseOverride(std::string_view data, int& index, ParkourActionOverride& parsedOverride)
    {
        const std::vector<std::string_view> fields = Split(data, ',');
        if (fields.size() != 8)
            return false;

        int overrideIndex = 0;
        int keyCode = 0;
        if (!TryParseInteger(fields[0], 10, overrideIndex)
            || !TryParseInteger(fields[1], 10, keyCode)
            || !IsValidVirtualKey(keyCode))
        {
            return false;
        }

        std::vector<int> actionTypes;
        for (const std::string_view actionTypeText : Split(fields[2], '|'))
        {
            if (actionTypeText.empty())
                continue;
            int actionType = 0;
            if (!TryParseInteger(actionTypeText, 10, actionType) || actionType < 0)
                return false;
            actionTypes.push_back(actionType);
        }
        if (actionTypes.empty())
            actionTypes.push_back(50); // Legacy files used the default type for an empty field.

        float fitnessMultiplier = 0.0f;
        bool removeOtherActions = false;
        bool enabled = false;
        bool toggleMode = false;
        bool toggleState = false;
        if (!TryParseFloat(fields[3], fitnessMultiplier)
            || !TryParseBool(fields[4], removeOtherActions)
            || !TryParseBool(fields[5], enabled)
            || !TryParseBool(fields[6], toggleMode)
            || !TryParseBool(fields[7], toggleState))
        {
            return false;
        }

        index = overrideIndex;
        parsedOverride.keyCode = keyCode;
        parsedOverride.actionTypes = std::move(actionTypes);
        parsedOverride.fitnessMultiplier = fitnessMultiplier;
        parsedOverride.removeOtherActions = removeOtherActions;
        parsedOverride.enable = enabled;
        parsedOverride.toggleMode = toggleMode;
        parsedOverride.toggleState = toggleState;
        parsedOverride.waitingForKey = false;
        return true;
    }

    // Some transient candidates have a NULL GetEnumParkourAction fancy slot.
    // Calling the member method on those candidates dispatches to address zero
    // and crashes, so validate the slot before reading an action type.
    int GetTypeIntSafe(const AvailableParkourAction* action)
    {
        if (!action || !action->fancyVTable)
            return -1;

        const FancyVFunction& slot =
            action->fancyVTable->fancyVirtuals[ParkourActionKnownFancyVFuncs::GetEnumParkourAction::idx];
        if (!slot.fn)
            return -1;

        return static_cast<int>(
            GET_AND_CAST_FANCY_FUNC(
                *action,
                ParkourActionKnownFancyVFuncs::GetEnumParkourAction)(
                    const_cast<AvailableParkourAction*>(action)));
    }
}

// ── AssassinAbilitySet helpers ────────────────────────────────────────

AssassinAbilitySet* FreeJumpPlugin::GetActiveAbilitySet()
{
    PlayerProgressionManager* mgr = PlayerProgressionManager::GetSingleton();
    if (!mgr) return nullptr;
    return mgr->assassinAbilitySets.GetHighestPrioritySet();
}

static std::string GetIniPath()
{
    char documents[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, nullptr, 0, documents)))
    {
        std::string folder(documents);
        folder += "\\Assassin's Creed Unity";
        if (!CreateDirectoryA(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            // The stream operation below will report the actual write failure.
        }
        folder += "\\FreeJumpPlugin.ini";
        return folder;
    }
    return "FreeJumpPlugin.ini";
}

void FreeJumpPlugin::LoadSettings()
{
    std::ifstream file(GetIniPath());
    if (!file) return;

    std::vector<ParkourActionOverride> loadedOverrides;
    bool hasOverrideCount = false;
    bool hasOverrideData = false;
    std::string line;
    while (std::getline(file, line))
    {
        const std::string_view trimmedLine = Trim(line);
        std::string_view value;

        if (TryGetValue(trimmedLine, "FreeJumpKey=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidVirtualKey(keyCode))
                m_ToggleKey = keyCode;
        }
        else if (TryGetValue(trimmedLine, "SnapDirectionKey=", value))
        {
            int keyCode = 0;
            if (TryParseInteger(value, 16, keyCode) && IsValidVirtualKey(keyCode))
                m_SnapDirectionKey = keyCode;
        }
        else if (TryGetValue(trimmedLine, "SnapDirectionMode=", value))
        {
            bool enabled = false;
            if (TryParseBool(value, enabled))
                m_SnapDirectionMode = enabled;
        }
        else if (TryGetValue(trimmedLine, "MouseLockEnabled=", value))
        {
            bool enabled = false;
            if (TryParseBool(value, enabled))
                m_MouseLockEnabled = enabled;
        }
        else if (TryGetValue(trimmedLine, "NumOverrides=", value))
        {
            int count = 0;
            if (TryParseInteger(value, 10, count) && count >= 0 && count <= kMaxOverrideCount)
            {
                if (!hasOverrideData)
                    loadedOverrides.assign(count, ParkourActionOverride{});
                else if (loadedOverrides.size() < static_cast<size_t>(count))
                    loadedOverrides.resize(count);
                hasOverrideCount = true;
                hasOverrideData = true;
            }
            else
            {
                // Preserve any valid Override records already read instead of
                // turning a malformed count into an empty configuration.
                hasOverrideCount = false;
            }
        }
        else if (TryGetValue(trimmedLine, "Override=", value))
        {
            int index = 0;
            ParkourActionOverride parsedOverride;
            if (!TryParseOverride(value, index, parsedOverride)
                || index < 0
                || index >= kMaxOverrideCount)
            {
                continue;
            }
            if (loadedOverrides.size() <= static_cast<size_t>(index))
                loadedOverrides.resize(static_cast<size_t>(index) + 1);
            loadedOverrides[static_cast<size_t>(index)] = std::move(parsedOverride);
            hasOverrideData = true;
        }
    }

    if (hasOverrideCount || hasOverrideData)
        m_Overrides = std::move(loadedOverrides);
}

void FreeJumpPlugin::SaveSettings()
{
    const std::string path = GetIniPath();
    const std::string temporaryPath = path + ".tmp";

    std::ofstream file(temporaryPath, std::ios::out | std::ios::trunc);
    if (!file)
        return;

    file << "FreeJumpKey=" << std::hex << m_ToggleKey << std::dec << "\n";
    file << "SnapDirectionKey=" << std::hex << m_SnapDirectionKey << std::dec << "\n";
    file << "SnapDirectionMode=" << (m_SnapDirectionMode ? 1 : 0) << "\n";
    file << "MouseLockEnabled=" << (m_MouseLockEnabled ? 1 : 0) << "\n";
    file << "NumOverrides=" << m_Overrides.size() << "\n";
    file << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (size_t i = 0; i < m_Overrides.size(); ++i)
    {
        const ParkourActionOverride& override = m_Overrides[i];
        file << "Override=" << i << "," << override.keyCode << ",";
        for (size_t actionIndex = 0; actionIndex < override.actionTypes.size(); ++actionIndex)
        {
            if (actionIndex > 0)
                file << "|";
            file << override.actionTypes[actionIndex];
        }
        file << "," << override.fitnessMultiplier
             << "," << (override.removeOtherActions ? 1 : 0)
             << "," << (override.enable ? 1 : 0)
             << "," << (override.toggleMode ? 1 : 0)
             << "," << (override.toggleState ? 1 : 0) << "\n";
    }

    file.flush();
    const bool flushSucceeded = file.good();
    file.close();
    if (!flushSucceeded || file.fail())
    {
        DeleteFileA(temporaryPath.c_str());
        return;
    }

    // Replace the old file only after the complete new file has been flushed.
    // This prevents a failed write from leaving a truncated configuration.
    if (!MoveFileExA(
            temporaryPath.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(temporaryPath.c_str());
    }
}

bool FreeJumpPlugin::SaveCurrentFlags()
{
    AssassinAbilitySet* set = GetActiveAbilitySet();
    if (!set) return false;
    m_SavedFlags.byte22 = *(uint8_t*)((uintptr_t)set + 0x22);
    m_SavedFlags.byte29 = *(uint8_t*)((uintptr_t)set + 0x29);
    m_SavedFlags.byte2A = *(uint8_t*)((uintptr_t)set + 0x2A);
    m_FlagsSaved = true;
    return true;
}

bool FreeJumpPlugin::RestoreFlags()
{
    if (!m_FlagsSaved) return true;
    AssassinAbilitySet* set = GetActiveAbilitySet();
    if (!set) return false;
    *(uint8_t*)((uintptr_t)set + 0x22) = m_SavedFlags.byte22;
    *(uint8_t*)((uintptr_t)set + 0x29) = m_SavedFlags.byte29;
    *(uint8_t*)((uintptr_t)set + 0x2A) = m_SavedFlags.byte2A;
    m_FlagsSaved = false;
    return true;
}

bool FreeJumpPlugin::ApplyMouseLock(bool enable)
{
    AssassinAbilitySet* set = GetActiveAbilitySet();
    if (!set) return false;
    if (enable)
    {
        if (!m_FlagsSaved && !SaveCurrentFlags())
            return false;
        uint8_t* byte22 = (uint8_t*)((uintptr_t)set + 0x22);
        *byte22 &= ~(1 << 2);
        uint8_t* byte29 = (uint8_t*)((uintptr_t)set + 0x29);
        *byte29 &= ~((1 << 1) | (1 << 4));
        uint8_t* byte2A = (uint8_t*)((uintptr_t)set + 0x2A);
        *byte2A &= ~(1 << 0);
        return true;
    }

    return RestoreFlags();
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

    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction*
    {
        return static_cast<FreeJumpPlugin*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.ChooseAfterSorting_fnp = nullptr;
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 5.0f;
    m_Callbacks.m_Name = "FreeJumpPlugin";

    if (m_MouseLockEnabled)
        m_LastMouseLockEnabled = ApplyMouseLock(true);
}

void FreeJumpPlugin::OnBeforeDeactivate()
{
    if (m_HookActive)
    {
        GenericHooksInParkourFiltering::GetSingleton()->Unsubscribe(m_Callbacks);
        m_HookActivator.reset();
        m_HookActive = false;
    }
    if (m_FlagsSaved)
    {
        ApplyMouseLock(false);
        m_LastMouseLockEnabled = false;
    }
    SaveSettings();
}

void FreeJumpPlugin::UpdateKeyState(int vkCode)
{
    if (!IsValidVirtualKey(vkCode) || m_KeyStateUpdatedThisFrame[vkCode])
        return;

    const bool keyIsDown = (GetAsyncKeyState(vkCode) & 0x8000) != 0;
    m_KeyPressedThisFrame[vkCode] = keyIsDown && !m_PreviousKeyDown[vkCode];
    m_PreviousKeyDown[vkCode] = keyIsDown;
    m_KeyStateUpdatedThisFrame[vkCode] = true;
}

void FreeJumpPlugin::UpdateAllKeyStates()
{
    for (int vkCode = 1; vkCode <= 0xFE; ++vkCode)
        UpdateKeyState(vkCode);
}

bool FreeJumpPlugin::WasKeyPressed(int vkCode) const
{
    return IsValidVirtualKey(vkCode) && m_KeyPressedThisFrame[vkCode];
}

bool FreeJumpPlugin::IsSortAndSelectSiteAlreadyPatched() const
{
    // ACUFixes-0.9.3 and ACU_ParkourPatches replace the sort/select function
    // at this exact address. A second replacement would restore the other
    // plugin's bytes when either DLL unloads, leaving a dangling or vanilla
    // function under an active hook.
    constexpr uintptr_t kSortAndSelectSite = 0x140133B00;
    const uint16_t firstBytes = *reinterpret_cast<const uint16_t*>(kSortAndSelectSite);
    return (firstBytes & 0x00FF) == 0x00E9  // near JMP rel32
        || firstBytes == 0x25FF              // JMP qword ptr [RIP+disp32]
        || firstBytes == 0xB848;             // MOVABS RAX, imm64 patch prologue
}

void FreeJumpPlugin::OnUpdate()
{
    m_KeyStateUpdatedThisFrame.fill(false);
    m_JustCapturedKey = 0;

    UpdateKeyState(m_ToggleKey);
    UpdateKeyState(m_SnapDirectionKey);
    for (const ParkourActionOverride& override : m_Overrides)
        UpdateKeyState(override.keyCode);

    const bool waitingForOverrideKey = std::any_of(
        m_Overrides.begin(),
        m_Overrides.end(),
        [](const ParkourActionOverride& override) { return override.waitingForKey; });
    if (m_WaitingForKey || m_WaitingForSnapKey || waitingForOverrideKey)
        UpdateAllKeyStates();

    const auto captureKey = [this](int& destinationKey) -> bool
    {
        for (int vkCode = 1; vkCode <= 0xFE; ++vkCode)
        {
            // The left/right/middle mouse buttons are used by the loader UI and
            // would otherwise be captured by the click that starts a rebind.
            if (vkCode <= VK_MBUTTON)
                continue;
            if (WasKeyPressed(vkCode))
            {
                destinationKey = vkCode;
                return true;
            }
        }
        return false;
    };

    if (m_WaitingForKey)
    {
        if (captureKey(m_ToggleKey))
        {
            m_JustCapturedKey = m_ToggleKey;
            m_WaitingForKey = false;
            SaveSettings();
        }
    }
    else if (m_WaitingForSnapKey)
    {
        if (captureKey(m_SnapDirectionKey))
        {
            m_JustCapturedKey = m_SnapDirectionKey;
            m_WaitingForSnapKey = false;
            SaveSettings();
        }
    }
    else if (waitingForOverrideKey)
    {
        for (ParkourActionOverride& override : m_Overrides)
        {
            if (!override.waitingForKey)
                continue;
            if (captureKey(override.keyCode))
            {
                m_JustCapturedKey = override.keyCode;
                override.waitingForKey = false;
                SaveSettings();
            }
            break;
        }
    }

    if (WasKeyPressed(m_SnapDirectionKey) && m_SnapDirectionKey != m_JustCapturedKey)
    {
        m_SnapDirectionMode = !m_SnapDirectionMode;
        SaveSettings();
    }

    for (ParkourActionOverride& override : m_Overrides)
    {
        if (!override.enable || !override.toggleMode)
            continue;
        if (WasKeyPressed(override.keyCode) && override.keyCode != m_JustCapturedKey)
        {
            override.toggleState = !override.toggleState;
            SaveSettings();
        }
    }

    if (m_MouseLockEnabled != m_LastMouseLockEnabled)
    {
        if (ApplyMouseLock(m_MouseLockEnabled))
            m_LastMouseLockEnabled = m_MouseLockEnabled;
    }

    const bool freeJumpHeld = m_ToggleKey != m_JustCapturedKey
        && (GetAsyncKeyState(m_ToggleKey) & 0x8000) != 0;
    bool anyOverrideActive = false;
    for (const ParkourActionOverride& override : m_Overrides)
    {
        const bool overrideKeyHeld = override.keyCode != m_JustCapturedKey
            && (GetAsyncKeyState(override.keyCode) & 0x8000) != 0;
        if (override.enable
            && (override.toggleMode
                ? override.toggleState
                : overrideKeyHeld))
        {
            anyOverrideActive = true;
        }
    }
    const bool needHook = freeJumpHeld || anyOverrideActive || m_SnapDirectionMode;

    if (needHook && !m_HookActive)
    {
        if (IsSortAndSelectSiteAlreadyPatched())
        {
            // Refuse a second full-function replacement. The UI reports this
            // state so the user can disable the conflicting parkour plugin.
            m_HookRefused = true;
        }
        else
        {
            m_HookRefused = false;
            auto gph = GenericHooksInParkourFiltering::GetSingleton();
            gph->Subscribe(m_Callbacks);
            m_HookActivator = gph->RequestGPHSortAndSelect();
            m_HookActive = true;
        }
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
    if (m_HookRefused)
    {
        ImGui::TextWrapped(
            "Parkour sort/select (0x140133B00) is already patched by another plugin. "
            "FreeJump hook was not installed. Disable/remove the conflicting parkour "
            "plugin (for example ACU_ParkourPatches.dll) before testing these features.");
    }
    if (ImGui::CollapsingHeader("Basic Features", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Free Jump:");
        ImGui::SameLine();
        ImGui::TextColored(freeMode ? activeColor : inactiveColor, freeMode ? "ACTIVE (HOLD)" : "OFF");
        ImGui::Text("  Bound key: 0x%02X", m_ToggleKey);
        if (m_WaitingForKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Input key to bind..."); }
        if (ImGui::Button(m_WaitingForKey ? "Cancel Rebind" : "Rebind##FreeJump"))
        {
            m_WaitingForKey = !m_WaitingForKey;
            if (m_WaitingForKey)
            {
                m_WaitingForSnapKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
            }
        }

        ImGui::Separator();
        ImGui::Text("Snap Direction:");
        ImGui::SameLine();
        ImGui::TextColored(snapMode ? ImVec4(0.20f,0.60f,0.90f,1.0f) : ImVec4(0.70f,0.70f,0.70f,1.0f),
                           snapMode ? "ON" : "OFF");
        ImGui::Text("  Toggle key: 0x%02X", m_SnapDirectionKey);
        if (m_WaitingForSnapKey) { ImGui::SameLine(); ImGui::TextColored(rebindColor, "Input key to bind..."); }
        if (ImGui::Button(m_WaitingForSnapKey ? "Cancel Rebind" : "Rebind##Snap"))
        {
            m_WaitingForSnapKey = !m_WaitingForSnapKey;
            if (m_WaitingForSnapKey)
            {
                m_WaitingForKey = false;
                for (ParkourActionOverride& override : m_Overrides)
                    override.waitingForKey = false;
            }
        }

        ImGui::Separator();
        bool mouseLock = m_MouseLockEnabled;
        if (ImGui::Checkbox("Disable Left/Right Mouse Buttons (sword/aim)", &mouseLock))
        { m_MouseLockEnabled = mouseLock; SaveSettings(); }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Parkour Action Overrides", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("+ Add Custom Override")
            && m_Overrides.size() < static_cast<size_t>(kMaxOverrideCount))
        {
            ParkourActionOverride newOverride;
            newOverride.keyCode = VK_F9; // sensible default, user rebinds after
            newOverride.waitingForKey = true; // start in bind mode
            m_WaitingForKey = false;
            m_WaitingForSnapKey = false;
            for (ParkourActionOverride& override : m_Overrides)
                override.waitingForKey = false;
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
                    if (ImGui::Checkbox("Enable", &ov.enable))
                        SaveSettings();
                    ImGui::SameLine();
                    ImGui::Text("Key: 0x%02X", ov.keyCode);
                    ImGui::SameLine();
                    if (ImGui::Button(ov.waitingForKey ? "Cancel" : "Rebind"))
                    {
                        const bool startWaiting = !ov.waitingForKey;
                        for (ParkourActionOverride& otherOverride : m_Overrides)
                            otherOverride.waitingForKey = false;
                        ov.waitingForKey = startWaiting;
                        if (startWaiting)
                        {
                            m_WaitingForKey = false;
                            m_WaitingForSnapKey = false;
                        }
                    }
                    if (ov.waitingForKey) ImGui::TextColored(rebindColor, "Input key to bind...");

                    bool holdMode = !ov.toggleMode;
                    if (ImGui::RadioButton("Hold (press and hold)", holdMode))
                    {
                        ov.toggleMode = false;
                        SaveSettings();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Toggle (press once to lock)", ov.toggleMode))
                    {
                        ov.toggleMode = true;
                        SaveSettings();
                    }

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
                    if (ImGui::DragFloat("Fitness Multiplier", &ov.fitnessMultiplier, 0.5f, 0.0f, 100000.0f))
                        SaveSettings();
                    if (ImGui::Checkbox("Remove all other actions", &ov.removeOtherActions))
                        SaveSettings();
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
                    GetTypeIntSafe(action)) == allowedTypes.end())
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
                    GetTypeIntSafe(action)) != ao.ptr->actionTypes.end())
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
