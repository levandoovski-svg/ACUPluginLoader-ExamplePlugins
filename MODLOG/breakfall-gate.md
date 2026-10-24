# MODLOG — Breakfall Gate (ACU.exe)
# Skill: reverse-engineering / REA
# Evidence preserved, not reconstructed from memory.

## Verified sources (never guessed)
- ACUFixes-0.9.3/src/ParkourDebugging/EnumParkourAction.h: breakfall=18, breakfall21=21
- ACUFixes-0.9.3/src/VariousPatches/Hack_BreakfallAndLedgeGrab.cpp: gate=0x141A3C0E1, epilogue=0x141A3C293
- ACUFixes-0.9.3/src/ParkourDebugging/LoggingTheHumanStates.cpp: InBreakfall/BreadkfallToHang enter=0x141A3BB20; hook base=0x1427555D2
- ACUFixes-0.9.3/src/ParkourDebugging/AvailableParkourAction.h: ledge distance / raycast context (GetTopLedgeDistance 0xD009E05C)
- ACUFixes-0.9.3/src/AnimationTools/DumpAnimationGraph.cpp: FreeFallState hash 0xeab9db2b, LandingState 0xb4eb28a5
- REBreakfall.json (prior attempt, session ses_e979c015cffeV9xiN61rzGo2Mh): unrelated — assassination/state hooks at 0x140CD9BED; no overlap with breakfall gate

## Binary / REA
- Target: D:\Games\Assassins Creed Unity\ACU.exe (30MB PE, 2015)
- Ghidra: D:\ghidra; install dir set; project /tmp/acu_ghidra_project analyzed headless (success)
- REA env: GHIDRA_INSTALL_DIR=/d/ghidra; opencode+pi registered; pyghidra OK; doctor showed provider not fully auto-wired (CLI fallback used per connection-recovery.md)
- Decompile of gate 0x141A3C0E1: present in Ghidra DB; C-like conditions not fully piped to CLI — requires GUI read (user explicitly declined to interpret)
- Blocking function identified BY ADDRESS (library-proven), not by user reading assembly

## Hook (proven pattern, not invented)
- Address 0x141A3C0E1: patch test to allow breakfall regardless of active move (library does this for "More reliable Breakfall")
- Address 0x141A3C293: epilogue patch (library second point)
- Context check still applies ledge range (AvailableParkourAction raycast) — safe because library preserves it
- No unrelated installs (hopper/ida/kuna not added)

## Unknown / residual
- Exact C conditions at 0x141A3C0E1 (e.g., which specific state flag is tested) known to Ghidra DB but not text-extracted here; can be read from Ghidra CodeBrowser at project /tmp/acu_ghidra_project
- Full decompile snippet of 0x141A3BB20 (InBreakfall enter) same status
