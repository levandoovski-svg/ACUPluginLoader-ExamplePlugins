#include "pch.h"
#include "GameThreadDeferredReset.h"

#include "AutoAssemblerKinda/AutoAssemblerKinda.h"

#include <memory>

namespace
{
    GameThreadDeferredReset g_Deferred;

    struct DeferredResetHook : AutoAssemblerCodeHolder_Base
    {
        DeferredResetHook()
        {
            PresetScript_CCodeInTheMiddle(0x141F3FE3B, 6,
                [](AllRegisters* /*params*/)
                {
                    g_Deferred.ServiceOnGameThread();
                });
        }
    };

    // The wrapper owns the trampoline, so it has to outlive Install().
    std::unique_ptr<AutoAssembleWrapper<DeferredResetHook>> g_Hook;
}

GameThreadDeferredReset& GameThreadDeferredReset::Get()
{
    return g_Deferred;
}

void GameThreadDeferredReset::Install()
{
    if (m_Installed)
    {
        return;
    }
    try
    {
        g_Hook = std::make_unique<AutoAssembleWrapper<DeferredResetHook>>();
        g_Hook->Activate();
        m_Installed = true;
    }
    catch (...)
    {
        g_Hook.reset();
        m_Installed = false;
    }
}

void GameThreadDeferredReset::ServiceOnGameThread()
{
    if (!m_Executor)
    {
        return;
    }
    __try
    {
        m_Executor();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
