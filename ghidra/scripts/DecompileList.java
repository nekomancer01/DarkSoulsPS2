// Decompiles the functions listed in a file (one hex entry address per line) to JSON lines {entry, c}.
// Runs one decompiler process per thread (ParallelDecompiler); decompiling only reads the program, so it scales
// with cores, unlike auto-analysis. Lines are written as each function finishes, in completion order.
// Args: <address list> <output .jsonl> [timeout seconds per function, default 60] [threads, default all cores]
//       [max characters per function, default 24000]
//@category DSR
import generic.concurrent.GThreadPool;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.util.task.TaskMonitor;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

public class DecompileList extends GhidraScript {
	private int maxChars = 24000;
	// Pool name used by ParallelDecompiler (ParallelDecompiler.THREAD_POOL_NAME is package-private).
	private static final String POOL = "Parallel Decompiler";

	private PrintWriter out;
	private int total;
	private int n;
	private long start;

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		int timeout = args.length > 2 ? Integer.parseInt(args[2]) : 60;
		int threads = args.length > 3 ? Integer.parseInt(args[3]) : Runtime.getRuntime().availableProcessors();
		if (args.length > 4) maxChars = Integer.parseInt(args[4]);
		out = new PrintWriter(args[1], "UTF-8");

		List<Function> functions = new ArrayList<>();
		for (String l : Files.readAllLines(Paths.get(args[0]))) {
			l = l.trim();
			if (l.isEmpty()) continue;
			Function f = getFunctionAt(toAddr(Long.parseUnsignedLong(l.replace("0x", ""), 16)));
			if (f == null) write(l, "// no function at " + l);
			else functions.add(f);
		}
		total = functions.size();
		start = System.currentTimeMillis();
		GThreadPool.getSharedThreadPool(POOL).setMaxThreadCount(threads);
		println("decompiling " + total + " functions on " + threads + " threads");

		DecompilerCallback<String[]> callback = new DecompilerCallback<>(currentProgram, d -> {
			d.toggleCCode(true);
			d.toggleSyntaxTree(true);
			d.setSimplificationStyle("decompile");
		}) {
			@Override
			public String[] process(DecompileResults r, TaskMonitor m) {
				String c = r.decompileCompleted() ? r.getDecompiledFunction().getC()
					: "// decompile failed: " + r.getErrorMessage();
				return new String[] { r.getFunction().getEntryPoint().toString(), c };
			}
		};
		callback.setTimeout(timeout);
		try {
			ParallelDecompiler.decompileFunctions(callback, currentProgram, functions.iterator(),
				res -> written(res), monitor);
		}
		finally {
			callback.dispose();
			out.close();
		}
		println("decompiled " + n + " functions");
	}

	private synchronized void written(String[] res) {
		if (res == null) return;
		write(res[0], res[1]);
		if (++n % 50 == 0 || n == total) {
			double per = (System.currentTimeMillis() - start) / 1000.0 / n;
			long left = (long) (per * (total - n));
			println(String.format("decompiled %d/%d (%.1f%%), %.3fs/fn, ETA %d:%02d:%02d", n, total,
				100.0 * n / total, per, left / 3600, left / 60 % 60, left % 60));
		}
	}

	private synchronized void write(String entry, String c) {
		if (c.length() > maxChars) c = c.substring(0, maxChars) + "\n// [truncated]";
		out.println("{\"entry\":\"" + entry + "\",\"c\":" + q(c) + "}");
		out.flush();  // keep finished functions if the run is killed; batch.sh imports partial output
	}

	// Same as ExportFunctions.q; Ghidra compiles each script on its own.
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
