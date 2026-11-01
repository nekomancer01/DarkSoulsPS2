// Writes one JSON line per function: entry, name, size, block, thunk flag, caller count, callees, referenced strings.
// No decompilation here (that is DecompileList.java, run per batch).
// Args: <output .jsonl>
//@category DSR
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class ExportFunctions extends GhidraScript {
	private static final int MAX_STRINGS = 24;
	private static final int MAX_STRING_LEN = 120;

	@Override
	public void run() throws Exception {
		PrintWriter out = new PrintWriter(getScriptArgs()[0], "UTF-8");
		int n = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (monitor.isCancelled()) break;
			Address entry = f.getEntryPoint();
			MemoryBlock blk = getMemoryBlock(entry);

			StringBuilder callees = new StringBuilder();
			for (Function c : f.getCalledFunctions(monitor)) {
				if (callees.length() > 0) callees.append(',');
				callees.append("[\"").append(c.getEntryPoint()).append("\",").append(q(c.getName(true))).append(']');
			}

			Set<String> strings = new LinkedHashSet<>();
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				if (strings.size() >= MAX_STRINGS) break;
				for (Reference ref : ins.getReferencesFrom()) {
					Data d = getDataAt(ref.getToAddress());
					if (d != null && d.hasStringValue()) {
						String s = String.valueOf(d.getValue());
						strings.add(s.length() > MAX_STRING_LEN ? s.substring(0, MAX_STRING_LEN) : s);
					}
				}
			}
			StringBuilder strs = new StringBuilder();
			for (String s : strings) {
				if (strs.length() > 0) strs.append(',');
				strs.append(q(s));
			}

			out.println("{\"entry\":\"" + entry + "\",\"name\":" + q(f.getName(true))
				+ ",\"size\":" + f.getBody().getNumAddresses()
				+ ",\"block\":\"" + (blk == null ? "?" : blk.getName() + "@" + blk.getStart()) + "\""
				+ ",\"thunk\":" + f.isThunk()
				+ ",\"n_callers\":" + f.getCallingFunctions(monitor).size()
				+ ",\"callees\":[" + callees + "],\"strings\":[" + strs + "]}");
			if (++n % 5000 == 0) println("exported " + n);
		}
		out.close();
		println("exported " + n + " functions");
	}

	static String q(String s) {
		StringBuilder b = new StringBuilder("\"");
		for (char c : s.toCharArray()) {
			switch (c) {
				case '"': b.append("\\\""); break;
				case '\\': b.append("\\\\"); break;
				case '\n': b.append("\\n"); break;
				case '\r': b.append("\\r"); break;
				case '\t': b.append("\\t"); break;
				default:
					if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
					else b.append(c);
			}
		}
		return b.append('"').toString();
	}
}
