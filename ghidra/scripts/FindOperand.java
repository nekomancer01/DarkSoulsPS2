// Lists instructions whose text contains a substring (e.g. an address an LEA loads that Ghidra made no reference for).
// Args: <substring>...
//@category DSR
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class FindOperand extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = getScriptArgs();
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Instruction ins = it.next();
			String s = ins.toString().toLowerCase();
			for (String x : a) {
				if (!s.contains(x.toLowerCase())) continue;
				Function f = getFunctionContaining(ins.getAddress());
				println(x + "  " + ins.getAddress() + "  " + ins + "  in " + (f == null ? "?" : f.getEntryPoint()));
			}
		}
	}
}
