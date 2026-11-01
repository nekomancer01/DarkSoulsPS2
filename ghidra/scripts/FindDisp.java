// Lists functions containing an instruction with mnemonic prefix M that has a memory operand with displacement D.
// Args: <mnemonic prefix, e.g. MOVZX> <displacement hex, e.g. 0xf2> [operand size substring, e.g. "word"]
//@category DSR
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import java.util.TreeMap;

public class FindDisp extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = getScriptArgs();
		String mn = a[0].toUpperCase();
		String disp = "+ " + a[1].toLowerCase() + "]";
		String size = a.length > 2 ? a[2].toLowerCase() : "";
		TreeMap<String, Integer> hits = new TreeMap<>();
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Instruction ins = it.next();
			if (!ins.getMnemonicString().toUpperCase().startsWith(mn)) continue;
			String s = ins.toString().toLowerCase();
			if (!s.contains(disp) || !s.contains(size)) continue;
			Function f = getFunctionContaining(ins.getAddress());
			String k = f == null ? "nofunc@" + ins.getAddress() : f.getEntryPoint().toString();
			hits.merge(k, 1, Integer::sum);
		}
		for (var e : hits.entrySet()) println(e.getKey() + " " + e.getValue());
		println(hits.size() + " functions");
	}
}
