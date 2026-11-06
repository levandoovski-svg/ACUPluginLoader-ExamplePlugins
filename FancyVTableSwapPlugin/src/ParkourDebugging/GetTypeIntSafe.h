#pragma once

// Shared null-safe type read (from FailedWallRunPlugin.cpp pattern).
// NEVER call action->GetEnumParkourAction() directly: type 52 tables can have
// a NULL fancy fn at idx 0x45 and GET_AND_CAST_FANCY_FUNC does no null check,
// so the raw call may dereference null.

#include "ACU/basic_types.h"
#include "AvailableParkourAction.h"
#include "EnumParkourAction.h"

static constexpr uint64 k_Type52FancyVTable_Runtime = 0x143A522F0;      // 1.5.0 verified
static constexpr uint64 k_Type52FancyVTable_TransientBase = 0x143A41930;

// Cheap committed-memory check (VirtualQuery). Used to gate every read of a
// fancy table: freed/reused/poisoned table pointers otherwise crash the very
// first dereference (the wild-call crash class we chased for two sessions).
inline bool IsReadableMemory(uint64 p, uint64 len)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (p + len) <= ((uint64)mbi.BaseAddress + mbi.RegionSize);
}

inline int GetTypeIntSafe(const AvailableParkourAction* action)
{
    if (!action) return -1;
    const uint64 table = (uint64)action->fancyVTable;
    if (!table) return -1;
    // The type slot (idx 0x45) sits at table offset 0x8A0..0x8C0 — require
    // readability before reading anything out of a possibly-poisoned table.
    if (!IsReadableMemory(table, 0x8C0)) return -1;
    const FancyVFunction& slot = action->fancyVTable->fancyVirtuals[ParkourActionKnownFancyVFuncs::GetEnumParkourAction::idx];
    if (!slot.fn)
    {
        if (table == k_Type52FancyVTable_Runtime ||
            table == k_Type52FancyVTable_TransientBase)
            return (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
        return -1;
    }
    return (int)GET_AND_CAST_FANCY_FUNC(*action, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)((AvailableParkourAction*)action);
}
