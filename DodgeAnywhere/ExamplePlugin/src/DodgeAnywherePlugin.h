#pragma once
#include <cstdint>
#include <vector>
#include <string>

class DodgeAnywherePlugin
{
public:
    void OnUpdate();
    void OnImGuiRender();

private:
    bool m_Enabled = true;
    bool m_WaitingForKey = false;
    int  m_Key = VK_SPACE;
    bool m_IsOverriding = false;

    // Scanner for AssassinTask (size unknown, but we'll capture 0x200 bytes)
    bool m_CaptureOut = false;
    bool m_CaptureIn  = false;
    uint8_t m_SnapshotOut[0x200];
    uint8_t m_SnapshotIn[0x200];
    std::vector<std::string> m_ScanResults;

    void CaptureSnapshot(uint8_t* buffer);
    void Compare();
};