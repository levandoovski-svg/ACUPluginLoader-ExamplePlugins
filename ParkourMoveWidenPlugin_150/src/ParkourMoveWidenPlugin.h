#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

class AvailableParkourAction;

// ParkourMoveWidenPlugin_150 - pick ANY parkour move from a dropdown and apply
// the proven "angle widen" mechanism to just that move (ACU 1.5.0).
//
// HOW IT WORKS (no game code is patched)
// --------------------------------------
// Every parkour action type owns one or more "fancy vtables" in the writable
// .data image; slot 0x45 of each returns the action's EnumParkourAction (the
// byte pattern is `mov eax, <enum>; ret`), and slot 0x70 is
// QuickCheckAcceptableDistanceRanges, the gate that consumes the widen fields.
// The plugin scans that table range once, groups the tables by action type, and
// repoints slot 0x70 of the SELECTED type's table(s) to a thunk that:
//   1) writes the widen fields on the action (cone + allowed windows),
//   2) calls the original check implementation (saved per slot).
// Because only that type's own tables are touched, no other move is affected and
// other plugins' mechanics still stack: Sidehop360's cave at the shared
// 0x1401B5890 is simply called through, and the swing plugins' own slot swaps
// are on different types.
//
// The override is hotkey-gated and every slot is restored when the mode is off,
// when another move is selected, or when the plugin is disabled.
//
// HONEST LIMIT: 118 of the 137 tables use the generic check 0x1401B5890, which
// is the one whose field semantics are verified (cone + absolute min/max
// windows). A handful of moves use their own check implementation; for those the
// panel says so ("custom check") because the same fields may not be read there.
class ParkourMoveWidenPlugin
{
public:
    void OnBeforeActivate();
    void OnUpdate();
    void OnImGuiRender();

    bool ShouldWiden() const;
    void ApplyOverrides(AvailableParkourAction& action);

    // Per-table slot work.
    bool InstallForSelectedMove();
    void RestoreAll();

    // Tables discovered in this image, grouped by action type.
    int TableCount() const { return m_TableCount; }
    int TableType(int i) const;
    uintptr_t TableAddress(int i) const;
    uintptr_t TableCheckImpl(int i) const;

    // Slots currently swapped (for the status panel).
    int InstalledSlotCount() const { return m_InstalledSlotCount; }
    uintptr_t InstalledTable(int i) const;
    bool InstalledIsCustomCheck(int i) const;

private:
    void LoadSettings();
    void SaveSettings();
    static std::string GetIniPath();
    void DiscoverTables();
    void ApplySelectedMoveChange();

public:
    // 0 = off, 1 = widen.
    int m_Mode = 0;
    int m_SelectedMove = 39;         // EnumParkourAction value; 39 = the sidehop

    int m_Hotkey = VK_F11;           // hold; F8 sidehop, F9 swing, F10 swing walls
    bool  m_WaitingForHotkey = false;
    bool  m_CaptureArmed = false;

    float m_MinCosAngle = -1.0f;     // -1 = any direction
    float m_HeightDiffMin = -10.0f;  // absolute window around the src height
    float m_HeightDiffMax = 10.0f;
    float m_HorizontalMax = 30.0f;   // absolute ceiling (vanilla is move-specific)
    float m_DistanceMax = 40.0f;

    // Discovery state (filled once at activation).
    enum { k_MaxTables = 256 };
    int m_TableCount = 0;
    uintptr_t m_TableAddr[k_MaxTables] = {};
    int       m_TableTypeArr[k_MaxTables] = {};
    uintptr_t m_TableCheck[k_MaxTables] = {};   // original slot 0x70 fn at discovery

    // Slot-swap state (indices into the discovered table list).
    enum { k_MaxSlots = 8 };
    int m_InstalledSlotCount = 0;
    int m_InstalledTableIndex[k_MaxSlots] = {};
    int m_InstalledThunk[k_MaxSlots] = {};

    int  m_InstalledMove = -1;       // which move the current swaps belong to
    bool m_InstallRefused = false;
    std::string m_InstallMessage;

    // Telemetry.
    long long m_TelChecksSeen = 0;
    long long m_TelOverrides = 0;
    float     m_TelLastMinCosAngleSeen = 0.0f;
};
