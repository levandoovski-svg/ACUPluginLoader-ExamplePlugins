#pragma once

// Shared null-safe type read (from FailedWallRunPlugin.cpp pattern).
// NEVER call action->GetEnumParkourAction() directly: type 52 tables can have
// a NULL fancy fn at idx 0x45 and GET_AND_CAST_FANCY_FUNC does no null check,
// so the raw call may dereference null.
#include "AvailableParkourAction.h"
#include "EnumParkourAction.h"

static constexpr uint64 k_Type52FancyVTable_Runtime = 0x143A522F0;      // 1.5.0 verified
static constexpr uint64 k_Type52FancyVTable_TransientBase = 0x143A41930;

inline int GetTypeIntSafe(const AvailableParkourAction* action)
{
    if (!action || !action->fancyVTable) return -1;
    const FancyVFunction& slot = action->fancyVTable->fancyVirtuals[ParkourActionKnownFancyVFuncs::GetEnumParkourAction::idx];
    if (!slot.fn)
    {
        if ((uint64)action->fancyVTable == k_Type52FancyVTable_Runtime ||
            (uint64)action->fancyVTable == k_Type52FancyVTable_TransientBase)
            return (int)EnumParkourAction::wallrunUpFromGroundFailed_mb;
        return -1;
    }
    return (int)GET_AND_CAST_FANCY_FUNC(*action, ParkourActionKnownFancyVFuncs::GetEnumParkourAction)((AvailableParkourAction*)action);
}
