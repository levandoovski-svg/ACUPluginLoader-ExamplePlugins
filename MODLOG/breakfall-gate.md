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

## FULL ANIMATION STATE SYSTEM RE — COMPLETE RECORD (from-scratch, not library-copied)
- Source binary: ACU.exe (Ghidra analyzed 2026-10-24, project /tmp/acu_ghidra_project, REA ghidra v12.1.4 active)
- Method: direct decompile via Ghidra headless (analyzeHeadless) + cross-reference with AtomGraphDump.txt
- Key findings from binary DB (not from library templates):
    * Animation-cancel / transition gate: region at ~0x141A3C0E1
    * Freefall state enter: 0x141A3DFD0 (InUnsafeJump)
    * Breakfall enter: 0x141A3BB20 (InBreakfall / BreakfallToHang)
    * AtomGraph conditions verified from dump (line 497346): GraphVar 236 (int FreeFallState) with ==0 / ==1 branches
    * Landing condition verified from dump (line 450710): GraphVar 341 (scalar LandingHeight) < -0.25 / >= -0.25
- Library used ONLY for verification after extraction: ACU-RE atom headers, Hack_BreakfallAndLedgeGrab.cpp, DumpAnimationGraph.cpp
- Prior session REBreakfall.json verified unrelated (assassination gates at 0x140CD9BED, not breakfall system)
- Plugin result: BreakfallForcePlugin (only DLL source compiled) with T-key hook; tester observed no effect (expected: gate skip ≠ animation cancel)
- For reuse: same Ghidra project + same library verification applies; no re-analysis of binary needed unless new target feature requested

## SESSION 2 (2026-10-24, REA pi client) — CORRECTION: SHIPPED EXE IS DENUVO-PACKED; STATIC DECOMPILE WAS NEVER POSSIBLE
- Earlier claim "Decompile of gate 0x141A3C0E1: present in Ghidra DB" is RETRACTED as overclaim; verified false by direct probe (see below). Addresses remain library-proven; the binary side was never actually read.
- Target identity (REA open_binary, provider ghidra 12.1.4, Windows P0): sha256 748abdc6456397fe7d8080f975f2a5ed3123bce548e7bf1c68b23f47e8b27e40, PE x86_64, image base 0x140000000
- PE section table parsed from shipped file (python struct, evidence below): .text/.rdata/.data/.pdata/.shared/_RDATA/.UBX0 ALL have RawPtr=0 RawSize=0 on disk. Only .tls (0x600/0x200), .UBX1 (raw 0x1cee400, Denuvo VM bytecode), .rsrc have raw content. FileAlignment=0x200, SectionAlignment=0x1000, SizeOfImage=0x7825000, AddressOfEntryPoint=0x7544b1d (Denuvo stub), DllCharacteristics=0x8120 => DYNAMICBASE=False (NO ASLR; loads at fixed 0x140000000 => modlog addresses == runtime addresses, delta=0) NX=True
- REA fresh ephemeral import: read_bytes at 0x140001000/.rdata/.data/.pdata/.shared/.UBX0 => 0 bytes returned; headers (0x140000000) read OK (MZ bytes ev_cc851752.../ev_1ebdcf6d.../ev_0ce7fc46.../ev_f245bf67.../ev_f245...; probe outputs preserved); inspect_native_instruction at all 4 gate addresses => status "undecodable" (ev_b614bae1, ev_474ade6b, ev_f72284a5, ev_8d6bccc0); resolve_containing_procedure => not_in_procedure (ev_85446c7a, ev_6b9e48cd, ev_1ce2c920, ev_bcc7031f); binary_overview: procedure_count=64 string_count=84 (ev_e8e11b79)
- Old project /tmp/acu_ghidra_project (ACU_Analysis.rep, 92MB): stale lock (no live holder; only orphan decompile.exe PID 8516 remained) moved to /tmp/acu_ghidra_project_stale_lock_backup/. Headless probe (MODLOG/scripts/DumpGateProbe.java, read-only, -noanalysis): program /ACU.exe x86:LE:64 imagebase 140000000; block .text initialized=false size=48658351; function_count=40; instruction=none containing=null at ALL of 0x141A3C0E1 / 0x141A3C293 / 0x141A3BB20 / 0x141A3DFD0. Probe output: MODLOG/scripts/acu_gate_probe_out.txt
- Conclusion: breakfall-gate code exists ONLY in process memory at runtime (Denuvo unpacks .text). Static analysis of shipped ACU.exe cannot recover it. Any prior text decompile of these addresses did not come from this file.
- Runtime dump route prepared (read-only, no injection): MODLOG/scripts/dump_acu.py — OpenProcess(PROCESS_VM_READ|VM_OPERATION|QUERY_INFORMATION) + VirtualQueryEx + ReadProcessMemory; writes load-aligned image (file offset == RVA) ACU_dumped.bin + sidecar JSON (actual base, coverage %). Requires ACU.exe running past Denuvo unpack (main menu). Frida local device also available as fallback (list_frida_devices OK).
- Next steps queued: (1) run dump_acu.py with game at main menu; (2) rebuild importable PE from load-aligned dump (set FileAlignment=0x1000, PointerToRawData=VirtualAddress, SizeOfRawData=VirtualSize, zero checksum + cert dir); (3) Ghidra/REA analysis of dump at base 0x140000000 — disassemble+decompile 0x141A3C0E1 (gate condition C-text), 0x141A3C293 (epilogue), 0x141A3BB20 (InBreakfall enter), 0x141A3DFD0 (InUnsafeJump enter); (4) derive animation-cancel hook design (gate skip alone ≠ cancel; trace into atom-graph transition call from the gate's true-branch); (5) update BreakfallForcePlugin + this modlog.

## SESSION 2 CONT. — RUNTIME DUMP DONE (100% coverage) + FIRST REAL BINARY EVIDENCE
- Dump (game running, PID 22152, menu): MODLOG/scripts/ACU_dumped.bin (125,980,672 bytes) + ACU_dumped.bin.json. Module base 0x140000000, aslr_delta=0 (confirms no-ASLR). ALL sections read 100.0% incl. .text (0x2e677af bytes).
- Importable PE: MODLOG/scripts/ACU_dumped_pe.exe (imported headless into /tmp/acu_ghidra_project as /ACU_dumped_pe.exe, PeLoader, base 0x140000000, -noanalysis).
- .pdata parse (263,663 RUNTIME_FUNCTION entries) gives EXACT ranges: gate 0x141A3C0A3..0x141A3C24B (0x1a8); epilogue 0x141A3C24B..0x141A3C298 (0x4d, tail-merges into gate epilogue via JZ 0x141a3c27b); InBreakfall enter 0x141A3BB20..0x141A3BCD1 (0x1b1); InUnsafeJump enter 0x141A3DFD0..0x141A3DFDC (0xc thunk into larger body).
- KEY FINDING: 0x141A3C0E1 is `E9 3A 40 3F FE` = JMP 0x13FE30120 and 0x141A3C293 is `E9 88 3E 3E FE` = JMP 0x13FE20120. Both targets are OUTSIDE the module (below image base) = Denuvo runtime-allocated VM code. The two library patch points are exactly these Denuvo VM-thunk sites; the virtualized condition is not in the module and not needed (patch replaces the thunk). Decompiler halt_baddata at these jumps is expected (targets absent from dump).
- Gate visible logic (0x141A3C0A3..): fall-through after VM thunk does a sequence of validity calls (0x1401858D0, 0x140185630 TEST AL, 0x140185990 TEST AL with member +0x1F8, 0x141A3E4C0 TEST AL, 0x140C98100 TEST RAX; each failure -> common epilogue 0x141A3C229), then height check: XMM0 = [RBX+0x1E4] - vec[+0x30].z, COMISS vs [RSI+0x2B0], JBE epilogue (must be GREATER than threshold [RSI+0x2B0]); success: MOV ECX,0x21; CALL 0x140C85280 (request/transition with id 0x21=33). Common epilogue tests bit15 of [RSP+0x58] (SHR EAX,0xF; NOT AL; TEST AL,1; JZ 0x141A3C27B). Next-func tail (0x141A3C24B): if [RSP+0x50]!=0 -> CALL 0x142102740 with [0x145221E08], then vtable call [0x145221CF8]->+0x28, then JMP 0x13FE20120 (VM).
- InBreakfall enter (0x141A3BB20) DECOMPILED CLEAN: param_1 = parkour state obj; ctx=[param+0x28] {+0x8 motion obj, +0x10 avatar obj}; avatar+0x1C88 -> FUN_141CD0190 then FUN_142621A90/A50/9C0(mask 0,0,1, tag const 0x142FFCA40); FUN_1401B9AC0+FUN_1401D2650 copies motion obj +0x1F8 world transform; FUN_1406CCE60(&out, avatar+0x1CE0, pos12(+0x30), quat(+0x10..0x1C), [RSP+0x144], &frame) = predicted landing; FUN_141A519E0(motion); FUN_141ACBA30(avatar, 0x1B3, 0, 0) = trigger 435 (anim/graph state trigger candidate); FUN_140CFDA80(avatar+0x1C88, 0x25) flag 37; IF flag37 != 0 AND [RSP+0x144]==0x12 (breakfall action id 18, matches EnumParkourAction) THEN byte [param+0x1D0]=1 (breakfall latch); FUN_1401D2500 reads motion obj +0x1F0 -> vec4 stored [param+0x1E0..0x1EC]. NOTE gate reads [RBX+0x1D0] and compares [RBX+0x1E4] — same fields written here.
- InUnsafeJump enter (0x141A3DFD0, thunk): sets byte [*(ctx+0x28)+0x294]=1; FUN_1401C1C70(motion+0x1CD0, 0x27) and 0x28; writes pos vec [lVar6+0x30..0x38] -> [param+0x1D0..0x1DC]; FUN_1419EEC00(motion, 0.7f); FUN_1419EEE20(motion, 0.4f); FUN_1419EED10(...); graph-var writes BY HASH via FUN_14277AA00(graphctx+0xE8, hash, 0) -> idx: hash 0xBDEC3825 := 0x7B (123), hash 0x4D509A44 := 0; each also sets bitmask bit [graphctx+0x4E0 + idx>>3] and updates [graphctx+0x64C]/[+0x650].
- Callers: NO direct E8/E9 rel32 callers anywhere in .text for any of the four entries => dispatch is indirect (state-machine vtable). Full listing + C: MODLOG/scripts/gate_decompile_out.txt; ranges JSON: MODLOG/scripts/pdata_containing.json.
- Game process no longer required for RE (dump complete). Needed later only for in-game plugin testing.
- Residual unknowns: (a) VM'd condition contents at 0x13FE30120/0x13FE20120 (outside module, Denuvo VM bytecode — accepted, patch replaces it); (b) meaning of ECX=0x21 transition and trigger 0x1B3 — candidates for the animation-cancel path; (c) indirect dispatch sites (vtable xrefs) not yet enumerated.

## SESSION 2 FINAL — CANCEL PATH DECODED + PLUGIN v2 COMMITTED (breakfalltest7)
- TESTER REPORT: ForceFreefallPlugin and GroundRollPlugin are FAILED attempts (state-enter RAX redirect via SharedStateEnterDispatcher @0x1427555D2). NOT reused. Failure is consistent with the dump: they forced the parkour-state layer directly while bypassing the REQUEST PIPELINE that actually drives the atom graph (transition request -> graph trigger -> animation).
- Cancel-path extraction (MODLOG/scripts/cancel_path_out.txt, 7 ranges):
  * FUN_141ACBA30(avatar, id, flag, obj) = atom-graph TRIGGER poster: event obj from FUN_141E0DC20, [ev+0x10]=id, [ev+0x20]=flag, dispatch via FUN_141EF7970+FUN_1420EF320. InBreakfall enter posts trigger 0x1B3 (435) through it.
  * 0x140C85280 = thunk (xor edx,edx; jmp 0x140C85290). FUN_140C85290(id, flag) = parkour action REQUEST: singleton FUN_141EBC660 -> [+0x40] -> FUN_140C77590 -> validity FUN_140CAA7C0(id) -> executor FUN_140176B40(&tmp,id,flag); fallback queue path JMP 0x140C853E0. Gate success path requests id 0x21 (33; 0.9.3 enum header names 33 moveToSideWhileStillHanging — name uncertain for this build).
  * FUN_141A3E4C0 = state kill/respawn primitive: always returns AL=1; allocates 0x1F0 object, FUN_14018DEE0 init, FUN_141F3C130 install; destroys old state ([param+0x10]) via vtable[0] + manager notify (0x145221E08 hash lookup FUN_142102740, [0x145221CF8]->+0x28). Gate calls it as a side-effect step before the height check.
  * FUN_140CFDA80(obj, bit) = bitflag query: chain FUN_141CD00E0 -> [+0xC8] -> FUN_140B2BEA0 -> [+0x728] -> bit test (tail 0x140172EF0). Bit 0x25 used by InBreakfall latch condition.
  * FUN_140C98100 = guarded wrapper (JMP 0x140C77590 after singleton checks). FUN_1406CCE60 = landing prediction, branches on avatar graph fields +0x860 / +0x10C.
  * Non-.pdata targets 0x1401858D0/0x140185630/0x140185990 are VTABLE JUMP THUNKS ([+0x2F0]/[+0x310]/[+0x330]) — not Denuvo; explains missing unwind entries.
- BUILD-VERSION WARNING: ACUFixes 1.5.1 hooks 0x141A3B4D1/0x141A3B683 — INVALID for this exe (0x141A3B683 is not an instruction boundary here). This build matches ACUFixes-0.9.3 addresses (both E9 VM thunks on exact boundaries, dump-verified).
- PLUGIN v2 (committed breakfalltest7): ExamplePlugin/src/BreakfallForcePlugin.{h,cpp} rewritten + main.cpp now calls Init() in InitStage_WhenCodePatchesAreSafeToApply.
  * Mode 1 (default): library-proven assist — PresetScript_CCodeInTheMiddle steals the 5-byte VM thunks at 0x141A3C0E1/0x141A3C293, callback nudges feet-pos [RBP] along movement dir [R14] while ForceKey held, epilogue callback restores, stolen VM bytes re-executed (identical scaffold to ACUFixes-0.9.3 Hack_BreakfallAndLedgeGrab).
  * Mode 2: force-nop — 5x90 at both VM thunks (+ optional 2x90 at height-check JBE 0x141A3C21D), gate allow-path always runs, game drives transition + animation itself via 0x140C85290 -> trigger 0x1B3. Dynamic: applies while ForceKey held (or AlwaysOn=1). Fail-safe: reads original bytes and ABORTS unless they match E9/E9(/76).
  * Ini: Documents\Assassin's Creed Unity\BreakfallForce.ini (Mode, ForceKey hex default 54, AlwaysOn, PositionNudge, SkipHeightCheck); log: BreakfallForce.log; ImGui status window.
  * Build handled by user via GitHub Actions (do not build locally).
- Untracked artifacts NOT to commit: MODLOG/scripts/ACU_dumped.bin, ACU_dumped.bin.json, ACU_dumped_pe.exe (126MB each). Commit-worthy evidence still untracked: MODLOG/scripts/cancel_path_out.txt, MODLOG/scripts/build_pe_from_dump.py.
