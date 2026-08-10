#pragma once
#include <cstdint>
#include <array>

class SidehopNopPlugin
{
public:
    SidehopNopPlugin();
    ~SidehopNopPlugin();

    void LoadSettings();
    void SaveSettings();
    void OnUpdate();
    void OnImGuiRender();

private:
    void ApplyNop();
    void RestoreBytes();

    bool m_Enabled = false;
    int  m_KeyBind = VK_F8;
    bool m_NopCurrentlyActive = false;

    std::array<uint8_t, 4> m_OriginalBytes{};
    bool m_HaveOriginalBytes = false;
    uintptr_t m_TargetAddress = 0;

    std::string GetIniPath();
};
