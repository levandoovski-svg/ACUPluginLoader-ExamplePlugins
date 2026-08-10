#include "pch.h"
#include "PrecisionParkourPlugin.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/SmallArray.h"
#include "ACU/Entity.h"
#include "ACU/Enum_MouseButtons.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/ACUPlayerCameraComponent.h"
#include "Common_Plugins/Enum_BindableKeyCode_Keyboard.h"
#include "Common_Plugins/ACU_InputUtils.h"
#include "Serialization/enumFactory.h"
#include "ParkourDebugging/AvailableParkourAction.h"
#include "ParkourDebugging/ParkourTester.h"
#include "vmath/vmath.h"
#include <shlobj.h>
#include <fstream>

static bool g_PrecisionModeEnabled = false;
static int  g_PrecisionKeyBind = 46;

DEFINE_GAME_FUNCTION(AvailableParkourAction__FinalFilter1, 0x1401D4360, bool, __fastcall, (AvailableParkourAction* p_parkourAction, __m128* a2, uint64 a3, Entity* p_playerEntity));
DEFINE_GAME_FUNCTION(AvailableParkourAction__FinalFilter2, 0x1401D2580, bool, __fastcall, (AvailableParkourAction* p_parkourAction, Entity* p_playerEntity, __int64 a3));
DEFINE_GAME_FUNCTION(SmallArray_POD__RemoveGeneric, 0x142726000, void, __fastcall, (void* smallArray, int p_idx, unsigned int p_elemSize));

static bool IsKeyPressed(int keyBind)
{
    if (keyBind >= 256)
        return ACU::Input::IsPressed(static_cast<MouseButton>(keyBind - 256));
    return ACU::Input::IsPressed(static_cast<BindableKeyCode_Keyboard>(keyBind));
}

static int SortAndSelectBestMatchingAction_Replacement(
    ParkourTester* parkourTester,
    __m128* p_locationOfOrigin,
    uint64 a3,
    __m128* p_directionOfMovementInputWorldSpace,
    float a5,
    int a6,
    char a7,
    __int64 a8,
    SmallArray<AvailableParkourAction*>& p_parkourSensorsResults)
{
    __try
    {
        for (int i = 0; i < p_parkourSensorsResults.size; i++)
        {
            AvailableParkourAction* action = p_parkourSensorsResults[i];
            if (!action) continue;
            float fitness = GET_AND_CAST_FANCY_FUNC(*action, ParkourActionKnownFancyVFuncs::GetFitness)(action);
            if (fabsf(fitness) <= 0.0000099999997f)
            {
                SmallArray_POD__RemoveGeneric(&p_parkourSensorsResults, i--, 8u);
            }
        }

        auto getTotalWeight = [&](AvailableParkourAction& action) -> float {
            float fitness = GET_AND_CAST_FANCY_FUNC(action, ParkourActionKnownFancyVFuncs::GetFitness)(&action);
            float defaultWeight = GET_AND_CAST_FANCY_FUNC(*parkourTester, ParkourTesterKnownFancyVFuncs::GetDefaultWeightForAction)(*parkourTester, action);
            return fitness * defaultWeight;
        };

        std::sort(p_parkourSensorsResults.begin(), p_parkourSensorsResults.end(),
            [&](AvailableParkourAction* a, AvailableParkourAction* b) {
                if (!a || !b) return false;
                return getTotalWeight(*a) > getTotalWeight(*b);
            });

        __m128 smthOut;
        GET_AND_CAST_FANCY_FUNC(*parkourTester, ParkourTesterKnownFancyVFuncs::ParkourTester_FancyVFunc_0x16)(&smthOut, *parkourTester, p_locationOfOrigin, p_directionOfMovementInputWorldSpace);

        bool precisionActive = g_PrecisionModeEnabled;
        if (precisionActive)
        {
            __try { precisionActive = IsKeyPressed(g_PrecisionKeyBind); } __except (EXCEPTION_EXECUTE_HANDLER) { precisionActive = false; }
        }

        if (precisionActive && p_parkourSensorsResults.size > 0)
        {
            ACUPlayerCameraComponent* cam = nullptr;
            __try { cam = ACU::GetPlayerCameraComponent(); } __except (EXCEPTION_EXECUTE_HANDLER) { cam = nullptr; }
            if (cam)
            {
                Vector3f camPos(cam->positionLookFrom.x, cam->positionLookFrom.y, cam->positionLookFrom.z);
                float angleZ = cam->spinaroundAngleZtarget;
                float angleV = cam->spinaroundAngleUpDownTarget;
                float cv = cosf(angleV);
                Vector3f camForward(cv * sinf(angleZ), sinf(angleV), cv * cosf(angleZ));
                const float RAY_THRESHOLD = 1.5f;

                int bestIdx = -1;
                float bestT = FLT_MAX;

                for (int i = 0; i < p_parkourSensorsResults.size; i++)
                {
                    AvailableParkourAction* action = p_parkourSensorsResults[i];
                    if (!action) continue;
                    if (!AvailableParkourAction__FinalFilter1(action, p_locationOfOrigin, a8, parkourTester->entity))
                        continue;
                    if (!AvailableParkourAction__FinalFilter2(action, parkourTester->entity, a8))
                        continue;

                    Vector3f actionDest(action->locationAnchorDest.x, action->locationAnchorDest.y, action->locationAnchorDest.z);
                    Vector3f toAction = actionDest - camPos;
                    float t = toAction.dotProduct(camForward);
                    if (t < 0.0f) continue;

                    float perpDist = (actionDest - (camPos + camForward * t)).length();
                    if (perpDist < RAY_THRESHOLD && t < bestT)
                    {
                        bestT = t;
                        bestIdx = i;
                    }
                }

                if (bestIdx >= 0)
                    return bestIdx;
            }
        }

        for (int i = 0; i < p_parkourSensorsResults.size; i++)
        {
            AvailableParkourAction* action = p_parkourSensorsResults[i];
            if (!action) continue;
            if (!AvailableParkourAction__FinalFilter1(action, p_locationOfOrigin, a8, parkourTester->entity))
                continue;
            if (!AvailableParkourAction__FinalFilter2(action, parkourTester->entity, a8))
                continue;
            return i;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    return -1;
}

struct PrecisionParkourHook : AutoAssemblerCodeHolder_Base
{
    PrecisionParkourHook()
    {
        PresetScript_ReplaceFunctionAtItsStart(0x140133B00, SortAndSelectBestMatchingAction_Replacement);
    }
};

static AutoAssembleWrapper<PrecisionParkourHook> g_hook;
static bool g_HookInstalled = false;

std::string PrecisionParkourPlugin::GetIniPath()
{
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docs)))
        return std::string(docs) + "\\Assassin's Creed Unity\\PrecisionParkour.ini";
    return "PrecisionParkour.ini";
}

void PrecisionParkourPlugin::LoadSettings()
{
    std::ifstream f(GetIniPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("Enabled=", 0) == 0)
            m_Enabled = (line.substr(8) == "1");
        else if (line.rfind("KeyBind=", 0) == 0)
            try { m_KeyBind = std::stoi(line.substr(8)); } catch (...) {}
    }
}

void PrecisionParkourPlugin::SaveSettings()
{
    try {
        std::ofstream f(GetIniPath());
        if (f)
            f << "Enabled=" << (m_Enabled ? 1 : 0) << "\n"
              << "KeyBind=" << m_KeyBind << "\n";
    } catch (...) {}
}

void PrecisionParkourPlugin::Initialize()
{
    if (!g_HookInstalled)
    {
        g_hook.Activate();
        g_HookInstalled = true;
    }
    g_PrecisionKeyBind = m_KeyBind;
    g_PrecisionModeEnabled = m_Enabled;
}

void PrecisionParkourPlugin::OnUpdate()
{
    g_PrecisionKeyBind = m_KeyBind;
    g_PrecisionModeEnabled = m_Enabled;
}

void PrecisionParkourPlugin::OnImGuiRender()
{
    if (!ImGui::CollapsingHeader("Precision Parkour"))
        return;

    ImGui::TextWrapped("Hold key to only select parkour actions closest to screen center.");

    bool enabled = m_Enabled;
    if (ImGui::Checkbox("Enable Precision Parkour", &enabled))
    {
        m_Enabled = enabled;
        g_PrecisionModeEnabled = m_Enabled;
        SaveSettings();
    }

    if (m_Enabled)
    {
        struct Entry { int value; const char* name; };
        static std::vector<Entry> s_Entries;
        static bool s_Init = false;
        if (!s_Init)
        {
            s_Entries.push_back({256, "Mouse Left"});
            s_Entries.push_back({257, "Mouse Right"});
            s_Entries.push_back({258, "Mouse Middle"});
            s_Entries.push_back({259, "Mouse 4"});
            s_Entries.push_back({260, "Mouse 5"});
            auto pairs = enum_reflection<BindableKeyCode_Keyboard>::GetAllPairs();
            for (auto& p : pairs)
                s_Entries.push_back({(int)p.value, p.name});
            s_Init = true;
        }

        int currentIdx = 0;
        for (int i = 0; i < (int)s_Entries.size(); i++)
        {
            if (s_Entries[i].value == m_KeyBind) { currentIdx = i; break; }
        }

        std::vector<const char*> itemNames;
        itemNames.reserve(s_Entries.size());
        for (auto& e : s_Entries) itemNames.push_back(e.name);

        ImGui::Text("Keybind");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        if (ImGui::Combo("##ppkeybind", &currentIdx, itemNames.data(), (int)itemNames.size()))
        {
            m_KeyBind = s_Entries[currentIdx].value;
            g_PrecisionKeyBind = m_KeyBind;
            SaveSettings();
        }
    }
}
