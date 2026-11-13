// Print the instructions of the functions containing the given addresses.
// Usage (headless post-script): DisasmAt.java <hexaddr> [<hexaddr> ...]
// @category PixCars
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class DisasmAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address a = toAddr(Long.parseLong(arg.replace("0x", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) {
                println("asm no function at " + a);
                continue;
            }
            for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true))
                println("asm " + i.getAddress() + "  " + i);
        }
    }
}
