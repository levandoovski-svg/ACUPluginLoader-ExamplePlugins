/* Targeted disassemble + decompile of exact .pdata-derived function ranges
 * from the runtime dump. Args: <outfile> then ranges as START:END (hex, no 0x).
 * Read-only w.r.t. the program DB semantics of this headless run (session-scoped).
 */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.*;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileRanges extends GhidraScript {

    private long parseHex(String s) {
        s = s.trim().replace("0x", "").replace("0X", "");
        return Long.parseUnsignedLong(s, 16);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args[0];
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);
        try {
            for (int i = 1; i < args.length; i++) {
                String[] parts = args[i].split(":");
                long start = parseHex(parts[0]);
                long end = parseHex(parts[1]);
                Address as = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(start);
                Address ae = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(end);
                pw.println("########## RANGE 0x" + Long.toHexString(start) + "..0x" + Long.toHexString(end) + " ##########");

                // linear disassembly within the exact range
                Address cur = as;
                int count = 0;
                while (cur != null && cur.compareTo(ae) < 0 && count < 100000) {
                    boolean ok = disassemble(cur);
                    Instruction ins = ok ? currentProgram.getListing().getInstructionAt(cur) : null;
                    if (ins == null) {
                        pw.println("0x" + cur + "  <disassemble failed>");
                        cur = cur.add(1);
                        continue;
                    }
                    count++;
                    cur = ins.getMaxAddress().next();
                }
                pw.println("disassembled_instructions=" + count);

                Function f = getFunctionAt(as);
                if (f == null) f = createFunction(as, null);
                if (f != null) {
                    f.setBody(new AddressSet(as, ae.subtract(1)));
                    DecompileResults res = di.decompileFunction(f, 240, monitor);
                    pw.println("decompile_completed=" + res.decompileCompleted()
                        + (res.getErrorMessage() != null && !res.getErrorMessage().isEmpty() ? " err=" + res.getErrorMessage() : ""));
                    if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
                        pw.println("----C-BEGIN----");
                        pw.println(res.getDecompiledFunction().getC());
                        pw.println("----C-END----");
                    }
                } else {
                    pw.println("create_function_failed");
                }

                // full listing for byte-exact patch design
                pw.println("----ASM-BEGIN----");
                cur = as;
                while (cur != null && cur.compareTo(ae) < 0) {
                    Instruction ins = currentProgram.getListing().getInstructionAt(cur);
                    if (ins == null) { pw.println("0x" + cur + " ??"); cur = cur.add(1); continue; }
                    StringBuilder sb = new StringBuilder();
                    try {
                        for (byte b : ins.getBytes()) sb.append(String.format("%02x", b));
                    } catch (ghidra.program.model.mem.MemoryAccessException e) {
                        sb.append("??????");
                    }
                    pw.println(cur + "  " + sb + "  " + ins.toString());
                    cur = ins.getMaxAddress().next();
                }
                pw.println("----ASM-END----");
                pw.flush();
            }
            pw.println("RANGES_DONE");
        } finally {
            pw.close();
            di.dispose();
        }
    }
}
