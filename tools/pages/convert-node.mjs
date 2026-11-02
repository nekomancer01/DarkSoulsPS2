// The in-browser converter's run (tools/pages/worker.js) under Node: the same Pyodide release, its 32-bit numpy and
// Pillow, the C helpers as WebAssembly side modules and the same environment, so its packs can be compared with a
// native `tools/convert.py` run on the same game folder (TECH_DEBT build-7: the browser's packs should be the same bytes).
//
//   node tools/pages/convert-node.mjs <game dir> <build dir> [--wasm DIR] [--only STEP...]
//   python3 tools/convert.py --compare <build dir> <native build dir>
//
// --wasm: the helpers from tools/native/build-wasm.sh (built into a temporary dir when left out; needs clang + wasm-ld).
// Pyodide comes from npm (pyodide@PYODIDE in tools/pages/build.py) into build/cache/pyodide-node, its packages from its
// CDN on first use (cached there too). The game folder is only read; packs go to <build dir>/data. No disc image.
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..", "..");
const args = process.argv.slice(2);
const opt = (k) => { const i = args.indexOf(k); if (i < 0) return null; const v = args[i + 1]; args.splice(i, 2); return v; };
let wasm = opt("--wasm");
const onlyAt = args.indexOf("--only");
const only = onlyAt >= 0 ? args.splice(onlyAt).slice(1) : null;
if (args.length !== 2) {
  console.error("usage: node tools/pages/convert-node.mjs <game dir> <build dir> [--wasm DIR] [--only STEP...]");
  process.exit(2);
}
const game = path.resolve(args[0]), build = path.resolve(args[1]);
const version = /^PYODIDE = "([^"]+)"/m.exec(fs.readFileSync(path.join(ROOT, "tools/pages/build.py"), "utf8"))[1];

// Pyodide from npm, cached per release
const cache = path.join(ROOT, "build", "cache", "pyodide-node", version);
const mod = path.join(cache, "node_modules", "pyodide", "pyodide.mjs");
if (!fs.existsSync(mod)) {
  fs.mkdirSync(cache, { recursive: true });
  execFileSync("npm", ["install", "--prefix", cache, "--no-save", `pyodide@${version}`], { stdio: "inherit" });
}
const { loadPyodide } = await import(pathToFileURL(mod).href);

// the converter's source tree as tools/pages/build.py zips it: tracked tools/*.py, tools/native C files, the Makefile,
// plus the wasm helpers, dated after their sources so the tools load them instead of compiling
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "convert-node-"));
try {
  if (!wasm) {
    wasm = path.join(tmp, "wasm");
    execFileSync(path.join(ROOT, "tools/native/build-wasm.sh"), [wasm], { stdio: "inherit" });
  }
  const src = path.join(tmp, "src");
  const files = execFileSync("git", ["ls-files", "tools", "ps2/proto/Makefile"], { cwd: ROOT, encoding: "utf8" })
    .split("\n").filter((f) => f === "ps2/proto/Makefile" || (f.split("/").length === 2 && f.endsWith(".py")) ||
      (f.startsWith("tools/native/") && /\.(c|h)$/.test(f)));
  for (const f of files) {
    fs.mkdirSync(path.dirname(path.join(src, f)), { recursive: true });
    fs.copyFileSync(path.join(ROOT, f), path.join(src, f));
  }
  const later = new Date(Date.now() + 3600e3);
  for (const m of ["lz4pack", "bcdecode", "simplify", "geom"]) {
    const so = path.join(src, "tools/native", `lib${m}.so`);
    fs.copyFileSync(path.join(wasm, `lib${m}.so`), so);
    fs.utimesSync(so, later, later);
  }
  fs.mkdirSync(build, { recursive: true });

  const py = await loadPyodide({ packageCacheDir: path.join(cache, "packages") });
  await py.loadPackage(["numpy", "pillow"]);
  for (const [dir, at] of [[src, "/src"], [game, "/game"], [build, "/build"]]) {
    py.FS.mkdirTree(at);
    py.FS.mount(py.FS.filesystems.NODEFS, { root: dir }, at);
  }
  py.globals.set("ONLY", only ? py.toPy(only) : null);
  // as worker.js: no /proc, no processes, no caches
  try {
    py.runPython(`
import os, sys, time
os.makedirs("/proc/self", exist_ok=True)
open("/proc/self/status", "w").write("VmRSS:\\t0 kB\\nVmHWM:\\t0 kB\\n")
sys.path.insert(0, "/src/tools")
os.environ["SIMPLIFY_PROCS"] = "1"
os.environ["SIMPLIFY_CACHE"] = "0"
os.environ["TEX_CACHE"] = "0"
os.environ["TMPDIR"] = "/tmp"
import convert
t = time.time()
convert.run("/game", "/build", only=ONLY, lean=True)
print(f"converted in {time.time() - t:.0f} s (Pyodide, numpy {__import__('numpy').__version__}, "
      f"Pillow {__import__('PIL').__version__})")
`);
  } catch (e) {  // the Python traceback only (the error object also carries Pyodide's minified source)
    console.error(String(e.message || e).slice(-4000));
    process.exitCode = 1;
  }
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
