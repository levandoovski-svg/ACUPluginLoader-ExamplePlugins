/* GhidraScript probe for the existing ACU_Analysis project DB.
 * Reports memory block initialization, function count, and decompiles the
 * breakfall-gate addresses if the DB actually contains analyzed code there.
 * Evidence-only probe: writes a report file, mutates nothing.
 */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpGateProbe extends GhidraScript {

    private long parseAddr(String s) {
        s = s.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
        return Long.parseUnsignedLong(s, 16);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "acu_gate_probe_out.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        try {
            pw.println("program=" + currentProgram.getName()
                + " path=" + currentProgram.getDomainFile().getPathname()
                + " language=" + currentProgram.getLanguageID()
                + " imagebase=" + currentProgram.getImageBase());

            Memory mem = currentProgram.getMemory();
            for (MemoryBlock b : mem.getBlocks()) {
                pw.println(String.format("block %-10s %s-%s initialized=%s size=%d",
                    b.getName(), b.getStart(), b.getEnd(), b.isInitialized(), b.getSize()));
            }

            FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
            int n = 0;
            while (fi.hasNext()) { fi.next(); n++; }
            pw.println("function_count=" + n);

            DecompInterface di = new DecompInterface();
            DecompileOptions opts = new DecompileOptions();
            di.setOptions(opts);
            di.openProgram(currentProgram);

            for (String a : args) {
                if (!a.contains("0x") && !a.contains("0X")) continue;
                long v = parseAddr(a);
                Address ad = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
                Instruction instr = currentProgram.getListing().getInstructionAt(ad);
                Function f = getFunctionContaining(ad);
                pw.println("=== addr=0x" + Long.toHexString(v)
                    + " instruction=" + (instr == null ? "none" : instr.toString())
                    + " containing=" + (f == null ? "null" : f.getName() + "@0x" + f.getEntryPoint().getUnsignedOffset()));
                if (f != null) {
                    DecompileResults res = di.decompileFunction(f, 180, monitor);
                    pw.println("decompile_completed=" + res.decompileCompleted()
                        + (res.getErrorMessage() != null ? " err=" + res.getErrorMessage() : ""));
                    if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
                        pw.println("----C-BEGIN----");
                        pw.println(res.getDecompiledFunction().getC());
                        pw.println("----C-END----");
                    }
                }
            }
            pw.println("PROBE_DONE");
        } finally {
            pw.close();
        }
    }
}
