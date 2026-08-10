#pragma once

#include <vector>

struct AllRegisters;

class SharedStateEnterDispatcher
{
public:
    using Callback = void(*)(AllRegisters* params);

    static void Initialize();
    static void Subscribe(Callback cb);
    static void Unsubscribe(Callback cb);
    static void OnStateEnter(AllRegisters* params);

private:
    static std::vector<Callback> s_Callbacks;
};
