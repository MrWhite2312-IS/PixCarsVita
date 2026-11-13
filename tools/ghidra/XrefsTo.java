// Print every reference to the given addresses, with the function each comes from.
// Usage (headless post-script): XrefsTo.java <hexaddr> [<hexaddr> ...]
// @category PixCars
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class XrefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address a = toAddr(Long.parseLong(arg.replace("0x", ""), 16));
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                println("xref " + a + " <- " + r.getFromAddress() + " " + r.getReferenceType()
                        + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
            }
        }
    }
}
