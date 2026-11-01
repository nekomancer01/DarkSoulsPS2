// Prints vtable entries of classes whose RTTI-labelled vftable matches a name filter.
// Args: <class name substring> <comma-separated byte offsets, e.g. 0x100,0x1a8>
//@category DSR
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class VtableSlots extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		String filter = args[0];
		String[] offs = args[1].split(",");
		SymbolIterator it = currentProgram.getSymbolTable().getSymbolIterator("vftable", true);
		while (it.hasNext()) {
			Symbol s = it.next();
			String full = s.getName(true);
			if (!full.contains(filter)) continue;
			StringBuilder b = new StringBuilder(full + " @ " + s.getAddress());
			for (String o : offs) {
				long off = Long.decode(o.trim());
				Address slot = s.getAddress().add(off);
				long target = currentProgram.getMemory().getLong(slot);
				Address t = toAddr(target);
				Function f = getFunctionAt(t);
				b.append("  [" + o + "] " + t + (f != null ? " " + f.getName() : ""));
			}
			println(b.toString());
		}
	}
}
