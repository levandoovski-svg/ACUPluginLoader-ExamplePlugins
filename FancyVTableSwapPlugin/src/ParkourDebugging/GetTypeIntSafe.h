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

// A fancy-fn pointer must be executable code in the ACU 1.5.0 image before we
// call it. Readable-but-garbage tables (freed/reused game data) otherwise
// produce a wild call from our own code — the crash class from both dumps
// (FancyVTableSwapPlugin.dll frames directly under the wild PC).
inline bool IsExecutableCode(uint64 p)
{
    if (p < 0x140000000 || p >= 0x148000000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) != 0;
}
constexpr uint64 k_TypeSlotOffset = 0x8A0; // idx 0x45 * 0x20 within the fancy table
// The ONE gate for "is this a plausible fancy table we may read/fn-call":
// committed readable through the type slot, and the type-slot fn pointer is
// real code. Doesn't prove it's the RIGHT table — only that calling it won't
// jump into garbage.
inline bool IsPlausibleFancyTable(uint64 table)
{
    if (!IsReadableMemory(table, k_TypeSlotOffset + 0x20)) return false;
    return IsExecutableCode(*(uint64*)(table + k_TypeSlotOffset));
}

inline int GetTypeIntSafe(const AvailableParkourAction* action)
{
    if (!action) return -1;
    const uint64 table = (uint64)action->fancyVTable;
    if (!table) return -1;
    // Readability alone is NOT enough: a readable garbage "table" holds a
    // garbage fn pointer, and calling it is the wild-call crash we chased.
    if (!IsPlausibleFancyTable(table)) return -1;
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
