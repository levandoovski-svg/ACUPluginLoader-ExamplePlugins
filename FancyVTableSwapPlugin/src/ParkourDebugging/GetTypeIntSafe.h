#pragma once

// Shared null-safe type read (from FailedWallRunPlugin.cpp pattern).
// NEVER call action->GetEnumParkourAction() directly: type 52 tables can have
// a NULL fancy fn at idx 0x45 and GET_AND_CAST_FANCY_FUNC does no null check,
// so the raw call may dereference null.

#include "ACU/basic_types.h"
#include "AvailableParkourAction.h"
#include "EnumParkourAction.h"
#include <unordered_map>

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
constexpr uint64 k_TypeSlotFnOffset = 0x8B0; // idx 0x45 * 0x20 + 0x10 (fn inside its entry)
// The ONE gate for "is this a plausible fancy table we may read/fn-call":
// committed readable through the type-slot fn pointer, and that fn pointer is
// either null (some tables, e.g. type-52, leave this slot empty — the type is
// then resolved by known table address) or real executable code. Doesn't
// prove it's the RIGHT table — only that a call won't jump into garbage.
inline bool IsPlausibleFancyTable(uint64 table)
{
    if (!IsReadableMemory(table, k_TypeSlotFnOffset + 8)) return false;
    const uint64 fn = *(uint64*)(table + k_TypeSlotFnOffset);
    return fn == 0 || IsExecutableCode(fn);
}

// fn pointer for a given fancy slot; 0 when the slot is not safely callable
// (null or not executable code — callers must skip the action, calling a
// null/garbage fn is exactly the wild-call crash we keep hitting).
inline uint64 GetCallableFancyFn(uint64 table, uint32 idx)
{
    const uint64 off = (uint64)idx * 0x20 + 0x10; // fn sits at +0x10 in its 0x20 entry
    if (!IsReadableMemory(table, off + 8)) return 0;
    const uint64 fn = *(uint64*)(table + off);
    return IsExecutableCode(fn) ? fn : 0;
}

inline int GetTypeIntSafe(const AvailableParkourAction* action)
{
    if (!action) return -1;
    const uint64 table = (uint64)action->fancyVTable;
    if (!table) return -1;
    // shortcut: vtable->type cache skips the VirtualQuery plausibility gate and
    // the type vfunc call entirely once a table is known. Table identity is
    // stable for real tables; only risk is a stale type for a table whose
    // memory got freed and reused between cycles - wrong branch at worst, never
    // a wild call (calls only happen on the cache-miss path). Upgrade if any
    // stale-misread ever matters.
    static std::unordered_map<uint64, int> s_TypeCache;
    if (auto it = s_TypeCache.find(table); it != s_TypeCache.end())
        return it->second;
    // Readability alone is NOT enough: a readable garbage "table" holds a
    // garbage fn pointer, and calling it is the wild-call crash we chased.
    if (!IsPlausibleFancyTable(table)) return -1;
    const FancyVFunction& slot = action->fancyVTable->fancyVirtuals[ParkourActionKnownFancyVFuncs::GetEnumParkourAction::idx];
    int type;
    if (!slot.fn)
    {
        if (table == k_Type52FancyVTable_Runtime ||
            table == k_Type52FancyVTable_TransientBase)
            type = (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
        else
            type = -1;
    }
    else
    {
        type = (int)GET_AND_CAST_FANCY_FUNC(*action, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)((AvailableParkourAction*)action);
    }
    if (type >= 0)
        s_TypeCache.emplace(table, type); // only trust real tables
    return type;
}
