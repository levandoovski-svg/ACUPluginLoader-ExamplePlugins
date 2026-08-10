#include "pch.h"

#include "Hack_PistolRange.h"

#include "ACU/WeaponComponent.h"
#include "ACU/Entity.h"
#include "ACU_DefineNativeFunction.h"

#include "ACU/ManagedPtrs/ManagedPtrs.h"

DEFINE_GAME_FUNCTION(Entity__Get_Human1C8, 0x140C17520, void*, __fastcall, (Entity* entity));
DEFINE_GAME_FUNCTION(Entity__Get_WeaponComponent, 0x140C1B330, WeaponComponent*, __fastcall, (Entity* weaponEntity));
DEFINE_GAME_FUNCTION(Human1C8__GetCurrentRangedWeaponShared_mb, 0x140C14C50, SharedPtrNew<Entity>**, __fastcall, (void* human1C8, SharedPtrNew<Entity>** p_out, char p_0forRangedWeapon));

WeaponComponent* FindCurrentRangedWeaponComponent(Entity& player)
{
    SharedPtrNew<Entity>* foundRangedWeapon = nullptr;
    void* human1C8 = Entity__Get_Human1C8(&player);
    if (!human1C8) { return nullptr; }
    Human1C8__GetCurrentRangedWeaponShared_mb(human1C8, &foundRangedWeapon, 0);
    Entity* wpnEntity = foundRangedWeapon->GetPtr();
    foundRangedWeapon->DecrementWeakRefcount();
    if (!wpnEntity) { return nullptr; }
    return Entity__Get_WeaponComponent(wpnEntity);
}

void WhenSearchingForRangedNPCTargetGettingRangedWeaponData_AdjustForWeaponType(AllRegisters* params)
{
    Entity* player = (Entity*)params->rcx_;
    WeaponComponent* wpnCpnt = FindCurrentRangedWeaponComponent(*player);
    if (!wpnCpnt) { return; }

    // Only affect pistols: check multiple possible type values
    uint32 rawType = (uint32)wpnCpnt->weaponCpntType;
    if (rawType != 0xB && rawType != 0x10 && rawType != 0x13)
        return;

    NetFightWeapon* netFightWpn = wpnCpnt->netFightWeapon->GetPtr();
    if (!netFightWpn) { return; }
    NetFightWeapon_18* rangedData = netFightWpn->magazineData;
    if (!rangedData) { return; }

    float* rangeOut = (float*)params->rbx_;
    *rangeOut = rangedData->range * 0.5f;
}

PistolRangeHack::PistolRangeHack()
{
    uintptr_t whenSearchingForRangedNPCTargetGettingRangedWeaponData = 0x141A04EB9;
    uintptr_t sameFunctionEpilogue = 0x141A04EC8;
    PresetScript_CCodeInTheMiddle(whenSearchingForRangedNPCTargetGettingRangedWeaponData, 5,
        WhenSearchingForRangedNPCTargetGettingRangedWeaponData_AdjustForWeaponType, sameFunctionEpilogue, false);
}
