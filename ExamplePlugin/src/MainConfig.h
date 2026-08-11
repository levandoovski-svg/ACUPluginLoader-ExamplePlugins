#pragma once

#include "Common_Plugins/Enum_BindableKeyCode.h"

namespace MainConfig {

void FindAndLoadConfigFileOrCreateDefault(const fs::path& filename);
void WriteToFile();

} // namespace MainConfig

#include "OLYAFSer/OLYAFSer.h"
#include "Serialization/EnumAdapter.h"
#include "Serialization/NumericAdapters.h"

#define ACM(varName, VarType, AdapterType, optionalDefaultValue) ADD_CONFIG_MEMBER(varName, VarType, AdapterType, optionalDefaultValue)
#define YACSTOR(SubclsName) YACONFIGSECTION_SUBCLASS_CTOR(SubclsName)
struct ConfigTop : YAConfigSection {
    YACSTOR(ConfigTop);
    struct Features : YAConfigSection {
        YACSTOR(Features);
        ACM(exampleConfig_infiniteAmmo, bool, BooleanAdapter, false);
        ACM(exampleConfig_batlampChargeModeButton, BindableKeyCode, EnumAdapter_template<BindableKeyCode>, BindableKeyCode::KEYBOARD_N);
    };
    ACM(features, Features, YAConfigSectionAdapter, );

    struct GearSetPerk : YAConfigSection {
        YACSTOR(GearSetPerk);
        ACM(enabled, bool, BooleanAdapter, false);
        ACM(setIndex, int, IntegerAdapter_template<int>, 0);       // 0 = Musketeer
        ACM(requiredPieces, int, IntegerAdapter_template<int>, 5); // how many set pieces must be worn
        ACM(perkTypeIndex, int, IntegerAdapter_template<int>, 0);  // 0 = bullet capacity
        ACM(perkAmount, int, IntegerAdapter_template<int>, 6);     // e.g. +6 bullets
    };
    ACM(gearSetPerk, GearSetPerk, YAConfigSectionAdapter, );
};
extern ConfigTop g_Config;
