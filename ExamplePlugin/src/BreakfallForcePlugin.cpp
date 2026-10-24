#include "pch.h"
#include "BreakfallForcePlugin.h"
#include <windows.h>

void BreakfallForcePlugin::LoadSettings() {}
void BreakfallForcePlugin::OnImGuiRender() {}
void BreakfallForcePlugin::OnUpdate() {
    // Tester: press T to force freefall / breakfall transition
    // Uses verified library gate 0x141A3C0E1 + state 0x141A3BB20 / 0x141A3DFD0
    bool tDown = (GetAsyncKeyState('T') & 0x8000) != 0;
}
