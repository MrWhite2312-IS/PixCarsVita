// Decompile the functions containing the given addresses to <outdir>/<addr>.c
// Usage (headless post-script): DecompileAt.java <outdir> <hexaddr> [<hexaddr> ...]
// @category PixCars
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.File;
import java.io.PrintWriter;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        out.mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.setOptions(new DecompileOptions());
        ifc.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) {
                // no function yet: create one at the address
                disassemble(a);
                f = createFunction(a, null);
            }
            if (f == null) {
                println("no function at " + a);
                continue;
            }
            DecompileResults r = ifc.decompileFunction(f, 300, monitor);
            String name = String.format("%08x.c", f.getEntryPoint().getOffset());
            try (PrintWriter w = new PrintWriter(new File(out, name), "UTF-8")) {
                w.println("// " + f.getName() + " @ " + f.getEntryPoint()
                        + " (asked " + a + ")");
                if (r.decompileCompleted()) {
                    w.print(r.getDecompiledFunction().getC());
                } else {
                    w.println("// decompile failed: " + r.getErrorMessage());
                }
            }
            println("wrote " + name);
        }
        ifc.dispose();
    }
}
