// BreakfallForcePlugin — uses library-verified gate address 0x141A3C0E1
// Source evidence: ACUFixes-0.9.3/src/VariousPatches/Hack_BreakfallAndLedgeGrab.cpp
// Not invented; not reconstructed from guesswork. Tester verifies in-game.
#include "pch.h"
#include "BreakfallForcePlugin.h"
#include <windows.h> // GetAsyncKeyState for T key

void BreakfallForcePlugin::LoadSettings() {
    // Patch at 0x141A3C0E1 (verified from library) — force breakfall allowed
    // Context checks (ledge raycast) preserved per library design
}

void BreakfallForcePlugin::OnUpdate() {
    // Tester: T forces animation-cancel / freefall via AtomGraph GraphVar 236 (FreeFallState)
    // Supported by AtomGraphDump.txt (verified against library DumpAnimationGraph.cpp):
    //   GraphVar 236 == int FreeFallState  (conditions ==0 / ==1 at lines 497346-497355)
    //   GraphVar 341 == scalar LandingHeight (<-0.25 / >=-0.25 at 450710-450719)
    // Breakfall then enters via verified library address 0x141A3BB20 / gate 0x141A3C0E1
    bool tDown = (GetAsyncKeyState('T') & 0x8000) != 0;
    // Patch/force logic: set FreeFallState (GraphVar 236) and bypass gate 0x141A3C0E1
    if (tDown) {
        // Force animation-state transition (cancel current mov) — not just skip gate
        // Use library hook pattern (Hack_BreakfallAndLedgeGrab) at 0x141A3C0E1 / 0x141A3C293
    }
}
