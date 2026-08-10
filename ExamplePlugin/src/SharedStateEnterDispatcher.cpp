#include "pch.h"
#include "SharedStateEnterDispatcher.h"
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"
#include <algorithm>

std::vector<SharedStateEnterDispatcher::Callback> SharedStateEnterDispatcher::s_Callbacks;

void SharedStateEnterDispatcher::OnStateEnter(AllRegisters* params)
{
    for (auto cb : s_Callbacks)
    {
        cb(params);
    }
}

struct StateEnterDispatcherHook : AutoAssemblerCodeHolder_Base
{
    StateEnterDispatcherHook()
    {
        PresetScript_CCodeInTheMiddle(
            0x1427555D2, 7, SharedStateEnterDispatcher::OnStateEnter,
            RETURN_TO_RIGHT_AFTER_STOLEN_BYTES, true);
    }
};

void SharedStateEnterDispatcher::Initialize()
{
    static AutoAssembleWrapper<StateEnterDispatcherHook> hookWrapper;
    hookWrapper.Activate();
}

void SharedStateEnterDispatcher::Subscribe(Callback cb)
{
    for (auto existing : s_Callbacks) {
        if (existing == cb) return;
    }
    s_Callbacks.push_back(cb);
}

void SharedStateEnterDispatcher::Unsubscribe(Callback cb)
{
    auto it = std::remove(s_Callbacks.begin(), s_Callbacks.end(), cb);
    if (it != s_Callbacks.end())
        s_Callbacks.erase(it, s_Callbacks.end());
}
