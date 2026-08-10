#pragma once

#include "ACU/ManagedPtrs/ManagedPtrs.h"
#include "ACU/Animation.h"

class MyPlayedAnimation
{
public:
    MyPlayedAnimation(const ACU::StrongRef<Animation>& animStrongRef, uint64 playStartTime)
        : m_playedAnimationStrongRef(animStrongRef)
        , m_LastChangeTimestamp(playStartTime)
        , m_LastChangeAnimTime(0)
    {}

    float GetDuration()
    {
        if (Animation* anim = m_playedAnimationStrongRef.GetPtr())
            return anim->Length;
        return 0;
    }

    bool IsPaused() { return m_IsPaused; }

private:
    friend class MyAnimationPlayer;
    ACU::StrongRef<Animation> m_playedAnimationStrongRef;
    uint64 m_LastChangeTimestamp;
    float m_LastChangeAnimTime;
    bool m_IsPaused = false;
};

class MyAnimationPlayer
{
public:
    void StartAnimation(ACU::StrongRef<Animation>& sharedAnim);
    uint64 GetAnimatorTime();
    void UpdateAnimations();

    void Pause(MyPlayedAnimation& anim);
    void Unpause(MyPlayedAnimation& anim);

    bool m_isLooping = true;
    float m_speedMult = 1.0f;

private:
    float CalculateAnimTime(MyPlayedAnimation& anim, uint64 animatorTime);
    std::optional<MyPlayedAnimation> m_playedAnim;
};

extern MyAnimationPlayer g_MyAnimationPlayer;
