#include "pch.h"

#include "Hack_ParkourDistanceOverride.h"

#include "ParkourDebugging/AvailableParkourAction.h"
#include "MyLog.h"
#include "ImGuiConfigUtils.h"

static DEFINE_LOGGER_CONSOLE_AND_FILE(DistanceOverrideLogger, "[DistanceOverride]");

ParkourDistanceOverride_Callbacks::ParkourDistanceOverride_Callbacks()
{
    m_Callbacks.ChooseBeforeFiltering_fnp = [](void* userData, SmallArray<AvailableParkourAction*>& actions) -> AvailableParkourAction*
    {
        return static_cast<ParkourDistanceOverride_Callbacks*>(userData)->ChooseBeforeFiltering(actions);
    };
    m_Callbacks.m_UserData = this;
    m_Callbacks.m_CallbackPriority = 0.0f;
    m_Callbacks.m_Name = "ParkourDistanceOverride (test)";
}

AvailableParkourAction* ParkourDistanceOverride_Callbacks::ChooseBeforeFiltering(SmallArray<AvailableParkourAction*>& actions)
{
    for (int i = 0; i < actions.size; i++)
    {
        AvailableParkourAction* action = actions[i];
        if ((int)action->GetEnumParkourAction() != m_TargetActionEnum)
            continue;

        // NOTE: InitialTestIfActionFits() already ran during this action's construction,
        // using whatever distanceMin/distanceMax the *game* originally set for it --
        // that's why it's already present in `actions` at all. We can't retroactively
        // re-run that native check with a tighter range, so we enforce the cap ourselves
        // here, directly against the action's actual computed `distance`.
        //
        // We still mirror the values into distanceMin/distanceMax in case any later
        // native code (e.g. animation curve / speed selection) re-reads those fields --
        // but don't rely on that alone to reject the candidate.
        action->distanceMin = m_DistanceMin;
        action->distanceMax = m_DistanceMax;

        const bool isOutOfRange = (action->distance < m_DistanceMin) || (action->distance > m_DistanceMax);
        if (isOutOfRange)
        {
            // Zeroing fitness makes the engine's own filtering logic discard this candidate
            // (see the "isDiscarded_becauseFitnessWeightTooLow" check in
            // SortAndSelectBestMatchingAction_FullReplacement_inner), the same mechanism
            // the game already uses to throw out near-zero-fitness actions.
            action->fitness = 0.0f;
        }

        if (m_LogToConsole)
        {
            LOG_DEBUG(DistanceOverrideLogger, "enum=%d distance=%.2f allowed=[%.2f, %.2f] -> %s\n",
                m_TargetActionEnum, action->distance, m_DistanceMin, m_DistanceMax,
                isOutOfRange ? "REJECTED" : "kept");
        }
    }
    // We're only filtering/mutating candidates in place here, never forcing a selection.
    return nullptr;
}

Hack_ParkourDistanceOverride::Hack_ParkourDistanceOverride()
    : m_GPH(GenericHooksInParkourFiltering::GetSingleton())
{
    m_Callbacks = std::make_unique<ParkourDistanceOverride_Callbacks>();
}

void Hack_ParkourDistanceOverride::OnBeforeActivate()
{
    m_GPH->Subscribe(m_Callbacks->m_Callbacks);
    // ChooseBeforeFiltering_fnp is consumed inside the "Sort and select" replacement,
    // so that's the hook we need active -- same as ParkourActionsExtraProcessing does.
    m_Activator_GPHSortAndSelect = m_GPH->RequestGPHSortAndSelect();
}

void Hack_ParkourDistanceOverride::OnBeforeDeactivate()
{
    m_Activator_GPHSortAndSelect.reset();
    m_GPH->Unsubscribe(m_Callbacks->m_Callbacks);
}

void Hack_ParkourDistanceOverride::DrawImGuiControls()
{
    // Quick numeric entry, as requested. If you'd rather pick the action by name instead
    // of memorizing numbers, EnumParkourAction already supports ImGui::DrawEnumPicker<EnumParkourAction>(...)
    // (see ImGuiConfigUtils.h) -- swap the InputInt below for that if you want a dropdown.
    EnumParkourAction currentEnum = (EnumParkourAction)m_Callbacks->m_TargetActionEnum;
    if (ImGui::DrawEnumPicker<EnumParkourAction>("Target action", currentEnum, ImGuiComboFlags_HeightLarge))
    {
        m_Callbacks->m_TargetActionEnum = (int)currentEnum;
    }
    ImGui::InputInt("Target action enum (raw)", &m_Callbacks->m_TargetActionEnum);

    ImGui::DragFloat("Distance Min", &m_Callbacks->m_DistanceMin, 0.05f, 0.0f, 20.0f, "%.2f m");
    ImGui::DragFloat("Distance Max", &m_Callbacks->m_DistanceMax, 0.05f, 0.0f, 20.0f, "%.2f m");
    if (m_Callbacks->m_DistanceMax < m_Callbacks->m_DistanceMin)
        m_Callbacks->m_DistanceMax = m_Callbacks->m_DistanceMin;

    ImGui::Checkbox("Log every candidate to console", &m_Callbacks->m_LogToConsole);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Logs the actual distance of every candidate matching the target action enum,\n"
            "and whether it was kept or rejected by the current [min, max] range.\n"
            "Useful for figuring out what distance value stepDownWaistHeight_mb (8)\n"
            "actually reports for those unnatural ~6.5m floaty jumps.");
    }
}
