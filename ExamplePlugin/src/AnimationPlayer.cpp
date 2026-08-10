#include "pch.h"
#include "AnimationPlayer.h"
#include "Log.h"

#include "ACU/World.h"
#include "ACU/Entity.h"
#include "ACU/HumanStatesHolder.h"
#include "ACU/AtomAnimComponent.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU_DefineNativeFunction.h"
#include "ACU/Memory/ACUAllocs.h"
#include "AnimationTools/AtomGraphControls.h"

// 1.5.0 addresses (matching ACUFixes-master)
DEFINE_GAME_FUNCTION(oneOfThoseFns_WhenStartActing, 0x141AC3040, __int64, __fastcall, (HumanStatesHolder* p_humanStates, char* a2, Animation* p_anim, uint32 a4, char a5, char a6));
DEFINE_GAME_FUNCTION(oneOfThoseFns_UpdateCinematicAnimationTime, 0x141B1A610, __int64, __fastcall, (HumanStatesHolder* p_humanStates, float p_time, uint32 p_bodyPartChannelIdx));

static AtomAnimComponent* GetPlayerAtomAnimComponent()
{
    PluginLog("GetPlayerAtomAnimComponent: START");
    Entity* player = ACU::GetPlayer();
    PluginLog("GetPlayerAtomAnimComponent: ACU::GetPlayer returned");
    if (!player) { PluginLog("GetPlayerAtomAnimComponent: player is null"); return nullptr; }
    int cpntIdx = player->cpntIndices_157.atomAnimCpnt;
    PluginLog("GetPlayerAtomAnimComponent: about to index cpnts_mb");
    auto* result = static_cast<AtomAnimComponent*>(player->cpnts_mb[cpntIdx]);
    PluginLog("GetPlayerAtomAnimComponent: END ok");
    return result;
}

MyAnimationPlayer g_MyAnimationPlayer;

uint64 MyAnimationPlayer::GetAnimatorTime()
{
    PluginLog("GetAnimatorTime: START");
    auto* world = World::GetSingleton();
    PluginLog("GetAnimatorTime: World::GetSingleton returned");
    if (!world) { PluginLog("GetAnimatorTime: world is null"); return 0; }
    auto ts = world->clockInWorldWithSlowmotion.currentTimestamp;
    PluginLog("GetAnimatorTime: END ok");
    return ts;
}

static void RestartAnimation_impl(Animation& anim)
{
    PluginLog("RestartAnimation_impl: START");
    auto* humanStates = HumanStatesHolder::GetForPlayer();
    PluginLog("RestartAnimation_impl: GetForPlayer returned");
    if (humanStates)
    {
        PluginLog("RestartAnimation_impl: about to call oneOfThoseFns_WhenStartActing");
        char a2 = 0;
        oneOfThoseFns_WhenStartActing(humanStates, &a2, &anim, 0, 1, 1);
        PluginLog("RestartAnimation_impl: oneOfThoseFns returned OK");
    }
    else
    {
        PluginLog("RestartAnimation_impl: humanStates is null");
    }
}

void MyAnimationPlayer::StartAnimation(ACU::StrongRef<Animation>& sharedAnim)
{
    PluginLog("StartAnimation: START");
    m_playedAnim.emplace(sharedAnim, GetAnimatorTime());
    if (Animation* anim = sharedAnim.GetPtr())
    {
        PluginLog("StartAnimation: about to call RestartAnimation_impl");
        RestartAnimation_impl(*anim);
    }
    else
    {
        PluginLog("StartAnimation: anim ptr is null");
    }
    PluginLog("StartAnimation: END");
}

static const uint32 hash_CinematicAnimationTime = 0x2cf6a276;

void MyAnimationPlayer::UpdateAnimations()
{
    if (!m_playedAnim) return;

    PluginLog("UpdateAnimations: START");
    auto* animCpnt = GetPlayerAtomAnimComponent();
    if (!animCpnt) { PluginLog("UpdateAnimations: animCpnt null"); return; }

    float currentAnimTime = CalculateAnimTime(*m_playedAnim, GetAnimatorTime());
    auto* graphEval = animCpnt->pD0;
    if (!graphEval) { PluginLog("UpdateAnimations: graphEval null"); return; }

    PluginLog("UpdateAnimations: about to SetGraphVariable");
    SetGraphVariable<float>(*graphEval, hash_CinematicAnimationTime, currentAnimTime);
    PluginLog("UpdateAnimations: END");
}

float MyAnimationPlayer::CalculateAnimTime(MyPlayedAnimation& anim, uint64 animatorTime)
{
    if (anim.IsPaused())
        return anim.m_LastChangeAnimTime;

    float timeElapsed = (animatorTime - anim.m_LastChangeTimestamp) / 30000.0f;
    float animTimeChange = timeElapsed * m_speedMult;
    float newAnimTime = anim.m_LastChangeAnimTime + animTimeChange;

    if (!m_isLooping)
    {
        if (newAnimTime < 0) newAnimTime = 0;
        else if (newAnimTime > anim.GetDuration()) newAnimTime = anim.GetDuration();
    }
    else
    {
        float duration = anim.GetDuration();
        if (newAnimTime > duration) newAnimTime = fmodf(newAnimTime, duration);
        else if (newAnimTime < 0) newAnimTime = duration - fmodf(-newAnimTime, duration);
    }
    return newAnimTime;
}

void MyAnimationPlayer::Pause(MyPlayedAnimation& anim)
{
    uint64 t = GetAnimatorTime();
    anim.m_LastChangeTimestamp = t;
    anim.m_LastChangeAnimTime = CalculateAnimTime(anim, t);
    anim.m_IsPaused = true;
}

void MyAnimationPlayer::Unpause(MyPlayedAnimation& anim)
{
    anim.m_LastChangeTimestamp = GetAnimatorTime();
    anim.m_IsPaused = false;
}
