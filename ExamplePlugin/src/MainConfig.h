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
};
extern ConfigTop g_Config;
