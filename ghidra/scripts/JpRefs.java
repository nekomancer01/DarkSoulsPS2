// For each '<hex address>\t<string>' line, writes '<function entry>\t<string>' for every function that references the
// string. extract.py strips whitespace, so references up to 16 bytes before the address count too.
// Args: <input .tsv> <output .tsv>
//@category DSR
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.HashSet;
import java.util.Set;

public class JpRefs extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		PrintWriter out = new PrintWriter(args[1], "UTF-8");
		int lines = 0, refs = 0;
		for (String l : Files.readAllLines(Paths.get(args[0]))) {
			int tab = l.indexOf('\t');
			if (tab < 0) continue;
			lines++;
			Address a = toAddr(Long.parseUnsignedLong(l.substring(0, tab), 16));
			Set<Function> fns = new HashSet<>();
			for (int back = 0; back <= 16 && fns.isEmpty(); back++) {
				for (Reference r : getReferencesTo(a.subtract(back))) {
					Function f = getFunctionContaining(r.getFromAddress());
					if (f != null) fns.add(f);
				}
			}
			for (Function f : fns) {
				out.println(f.getEntryPoint() + "\t" + l.substring(tab + 1));
				refs++;
			}
		}
		out.close();
		println(lines + " string addresses, " + refs + " function references");
	}
}
