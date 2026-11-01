// Prints the functions that reference an address (e.g. a vtable, to find constructors).
// Args: <hex address>
//@category DSR
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class RefsTo extends GhidraScript {
	@Override
	public void run() throws Exception {
		for (String a : getScriptArgs()) {
			for (Reference r : getReferencesTo(toAddr(Long.parseUnsignedLong(a.replace("0x", ""), 16)))) {
				Function f = getFunctionContaining(r.getFromAddress());
				println(a + " <- " + r.getFromAddress() + " " + r.getReferenceType() + " in "
					+ (f == null ? "?" : f.getEntryPoint() + " " + f.getName()));
			}
		}
	}
}
