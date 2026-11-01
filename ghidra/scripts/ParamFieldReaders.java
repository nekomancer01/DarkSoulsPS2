// Lists code that reads a field of the NpcParam-layout row behind the ChrIns param block (vtable +0x100 -> +8):
// after each CALL [reg + 0x100], follows the returned pointer through "MOV reg,[RAX + 0x8]" and prints the
// displacements read from that register within the next instructions.
// Args: [displacement filter, e.g. 0xc] [instructions to scan, default 12]
//@category DSR
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class ParamFieldReaders extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = getScriptArgs();
		String filter = a.length > 0 ? "+ " + a[0].toLowerCase() + "]" : null;
		int window = a.length > 1 ? Integer.parseInt(a[1]) : 12;
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Instruction ins = it.next();
			if (!ins.getMnemonicString().equals("CALL") || !ins.toString().contains("+ 0x100]")) continue;
			String row = null;
			Instruction n = ins.getNext();
			for (int i = 0; i < window && n != null; i++, n = n.getNext()) {
				String s = n.toString();
				if (row == null) {
					if (s.startsWith("MOV ") && s.contains("[RAX + 0x8]")) row = s.substring(4, s.indexOf(','));
					continue;
				}
				if (s.contains("[" + row + " + ") && (filter == null || s.toLowerCase().contains(filter))) {
					Function f = getFunctionContaining(n.getAddress());
					println((f == null ? "?" : f.getEntryPoint().toString()) + " " + n.getAddress() + "  " + s);
				}
				if (n.getMnemonicString().equals("RET") || n.getMnemonicString().equals("CALL")) break;
			}
		}
	}
}
