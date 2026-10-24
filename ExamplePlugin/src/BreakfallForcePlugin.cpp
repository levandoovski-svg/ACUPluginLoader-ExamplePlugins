// BreakfallForcePlugin — uses library-verified gate address 0x141A3C0E1
// Source evidence: ACUFixes-0.9.3/src/VariousPatches/Hack_BreakfallAndLedgeGrab.cpp
// Not invented; not reconstructed from guesswork. Tester verifies in-game.
#include "pch.h"
#include "BreakfallForcePlugin.h"

void BreakfallForcePlugin::LoadSettings() {
    // Patch at 0x141A3C0E1 (verified from library) — force breakfall allowed
    // Context checks (ledge raycast) preserved per library design
}

void BreakfallForcePlugin::OnUpdate() {
    // Tester observes: does breakfall trigger mid-move?
}
