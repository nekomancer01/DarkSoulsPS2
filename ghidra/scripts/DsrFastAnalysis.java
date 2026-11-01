// Pre-script: turn off the slowest analyzers before auto-analysis of DarkSoulsRemastered.exe.
// They can be run later on selected functions.
//@category DSR
import ghidra.app.script.GhidraScript;

public class DsrFastAnalysis extends GhidraScript {
	private static final String[] OFF = {
		"Decompiler Parameter ID",
		"Aggressive Instruction Finder",
		"Non-Returning Functions - Discovered",
		"Condense Filler Bytes",
		"Embedded Media",
	};

	@Override
	public void run() throws Exception {
		java.util.Map<String, String> opts = getCurrentAnalysisOptionsAndValues(currentProgram);
		for (String name : OFF) {
			if (opts.containsKey(name)) {
				setAnalysisOption(currentProgram, name, "false");
				println("disabled: " + name);
			} else {
				println("not present: " + name);
			}
		}
	}
}
