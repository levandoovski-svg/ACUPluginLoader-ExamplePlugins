#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

class AvailableParkourAction;

// ParkourMultiWidenPlugin_150 - tick ANY number of parkour moves in a list and
// apply the proven "angle widen" to all of them at once (ACU 1.5.0, no ASLR).
//
// HOW IT WORKS (no game code is patched)
// --------------------------------------
// Every parkour action type owns one or more "fancy vtables" in writable .data;
// slot 0x45 returns the action's EnumParkourAction (bytes `mov eax,<enum>; ret`)
// and slot 0x70 is QuickCheckAcceptableDistanceRanges, which consumes the widen
// fields. The plugin discovers all 125 parkour tables once, then repoints slot
// 0x70 of every table whose type is ticked to ONE shared thunk.
//
// The thunk cannot know which table called it from its arguments, so it looks
// the calling table up by `action->fancyVTable` and calls that slot's saved
// original implementation. Calls are rare (per candidate check), so the linear
// lookup is irrelevant, and it keeps multi-table moves (46/9/11) correct even
// when their tables use different implementations.
//
// The override is hotkey-gated. Swaps are restored when the widen is turned
// off, when a plugin-owned table disappears, or when the tick set changes (the
// install path restores first and then re-swaps the new set).
//
// Coexists with: Sidehop360 (its cave at the shared 0x1401B5890 is called
// through), the swing widen plugins (different types), FreeForm (its sites are
// untouched).
class ParkourMultiWidenPlugin
{
public:
    void OnBeforeActivate();
    void OnUpdate();
    void OnImGuiRender();

    bool ShouldWiden() const;
    void ApplyOverrides(AvailableParkourAction& action);

    bool InstallForEnabledMoves();
    void RestoreAll();

    // Discovery results.
    int  TableCount() const { return m_TableCount; }
    int  TableType(int i) const { return (i >= 0 && i < m_TableCount) ? m_TableTypeArr[i] : -1; }
    uintptr_t TableCheckImpl(int i) const { return (i >= 0 && i < m_TableCount) ? m_TableCheck[i] : 0; }

    int InstalledSlotCount() const { return m_InstalledSlotCount; }
    bool IsMoveEnabled(int move) const;
    void SetMoveEnabled(int move, bool enabled);
    int  EnabledMoveCount() const;

private:
    void LoadSettings();
    void SaveSettings();
    static std::string GetIniPath();
    void DiscoverTables();
    void ComputeEnabledBits(uint64_t bits[2]) const;
    bool EnabledBitsChanged() const;

public:
    // 0 = off, 1 = widen.
    int m_Mode = 0;

    int m_Hotkey = VK_F11;           // hold; F8/F9/F10 belong to the other plugins
    bool  m_WaitingForHotkey = false;
    bool  m_CaptureArmed = false;

    float m_MinCosAngle = -1.0f;     // -1 = accept from any direction
    float m_HeightDiffMin = -10.0f;  // ABSOLUTE window around the src height
    float m_HeightDiffMax = 10.0f;
    float m_HorizontalMax = 30.0f;
    float m_DistanceMax = 40.0f;

    enum { k_MaxMoves = 128 };
    bool m_MoveEnabled[k_MaxMoves] = {};

    // Tables discovered in this image.
    enum { k_MaxTables = 256 };
    int       m_TableCount = 0;
    uintptr_t m_TableAddr[k_MaxTables] = {};
    int       m_TableTypeArr[k_MaxTables] = {};
    uintptr_t m_TableCheck[k_MaxTables] = {};

    // Installed slots.
    int m_InstalledSlotCount = 0;
    uint64_t m_InstalledBits[2] = {};
    int m_InstalledMode = 0;         // m_Mode value the last install attempt ran under
    std::string m_InstallMessage;

    // Telemetry.
    long long m_TelChecksSeen = 0;
    long long m_TelOverrides = 0;
    long long m_TelMissedTableLookups = 0;
    float     m_TelLastMinCosAngleSeen = 0.0f;

    // UI filter buffer.
    char m_Filter[64] = {};
};
