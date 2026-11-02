// The in-browser converter's run (tools/pages/worker.js) under Node: the same Pyodide release, its 32-bit numpy and
// Pillow, the C helpers as WebAssembly side modules and the same environment, so its packs can be compared with a
// native `tools/convert.py` run on the same game folder (TECH_DEBT build-7: the browser's packs should be the same bytes).
//
//   node tools/pages/convert-node.mjs <game dir> <build dir> [--wasm DIR] [--prof F.tsv] [--old-rule] [--only STEP...]
//   python3 tools/convert.py --compare <build dir> <native build dir>
//
// --wasm: the helpers from tools/native/build-wasm.sh (built into a temporary dir when left out; needs clang + wasm-ld).
// Pyodide comes from npm (pyodide@PYODIDE in tools/pages/build.py) into build/cache/pyodide-node, its packages from its
// CDN on first use (cached there too). The game folder is only read; packs go to <build dir>/data. No disc image.
// --prof: per step, seconds and the wasm heap after it (HEAP8.length, which only grows: the step's high-water mark),
// also printed as "[prof]" lines; export_map.py prints the heap per stage ("mem ..."). --old-rule: restart only past
// RESTART_MB, not before each map export (worker.js before 2026-11-02), for A/B.
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";

// V8 reserves ~10 GB of address space per wasm memory for its out-of-bounds trap handler, which an address-space cap
// (tools/q's prlimit --as) refuses ("WebAssembly.Memory(): could not allocate memory"): run without it, with explicit
// bounds checks instead (re-executed with the flag)
if (!process.execArgv.includes("--disable-wasm-trap-handler")) {
  const { spawnSync } = await import("node:child_process");
  const r = spawnSync(process.execPath, ["--disable-wasm-trap-handler", ...process.execArgv, ...process.argv.slice(1)],
    { stdio: "inherit" });
  process.exit(r.status ?? 1);
}

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..", "..");
const args = process.argv.slice(2);
const opt = (k) => { const i = args.indexOf(k); if (i < 0) return null; const v = args[i + 1]; args.splice(i, 2); return v; };
let wasm = opt("--wasm");
const profOut = opt("--prof");
const oldRule = (() => { const i = args.indexOf("--old-rule"); if (i < 0) return false; args.splice(i, 1); return true; })();
const onlyAt = args.indexOf("--only");
const only = onlyAt >= 0 ? args.splice(onlyAt).slice(1) : null;
if (args.length !== 2) {
  console.error("usage: node tools/pages/convert-node.mjs <game dir> <build dir> [--wasm DIR] [--prof F.tsv] [--old-rule] [--only STEP...]");
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
const rows = [];  // --prof
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

  // as worker.js: no /proc, no processes, no caches, and a fresh Pyodide before each map export (unless the heap is
  // still under MAP_START_MB) and once the last one's memory (which never shrinks) has grown past RESTART_MB, or a
  // map export runs into the 4 GB wasm32 ceiling. /build is on disk here, so the next Pyodide resumes at the step index.
  const RESTART_MB = 1536, MAP_START_MB = oldRule ? 1e9 : 400;
  let seg = 0;
  const t = Date.now();
  try {
    let start = 0, py;
    while (start !== null) {
      py = await loadPyodide({ packageCacheDir: path.join(cache, "packages") });
      await py.loadPackage(["numpy", "pillow"]);
      for (const [dir, at] of [[src, "/src"], [game, "/game"], [build, "/build"]]) {
        py.FS.mkdirTree(at);
        py.FS.mount(py.FS.filesystems.NODEFS, { root: dir }, at);
      }
      py.globals.set("ONLY", only ? py.toPy(only) : null);
      py.globals.set("START", start);
      py.globals.set("RESTART_MB", RESTART_MB);
      py.globals.set("MAP_START_MB", MAP_START_MB);
      const heapMB = globalThis.heapMB = () => py._module.HEAP8.length / 1048576;  // export_map.rss() prints it
      const segNo = seg++;
      let last = null;
      globalThis.profStep = (i, name) => {  // before each step, and with name null at the end of this Pyodide's run
        const now = Date.now();
        if (last) {
          const r = { seg: segNo, i: last.i, name: last.name, s: (now - last.t) / 1000, heap: heapMB() };
          rows.push(r);
          console.log(`[prof] ${r.s.toFixed(1)} s, heap ${r.heap.toFixed(0)} MB: ${r.name}`);
        }
        last = name === null ? null : { i, name, t: now };
      };
      start = py.runPython(`
import os, sys
os.makedirs("/proc/self", exist_ok=True)
open("/proc/self/status", "w").write("VmRSS:\\t0 kB\\nVmHWM:\\t0 kB\\n")
sys.path.insert(0, "/src/tools")
os.environ["SIMPLIFY_PROCS"] = "1"
os.environ["SIMPLIFY_CACHE"] = "0"
os.environ["TEX_CACHE"] = "0"
os.environ["TMPDIR"] = "/tmp"
import convert
from js import heapMB, profStep
_, nxt = convert.run_part("/game", "/build", only=ONLY, lean=True, start=START,
                          progress=lambda i, n, name: profStep(i, name),
                          pause=lambda nxt: heapMB() > RESTART_MB or
                          (convert.is_map_export(nxt) and heapMB() > MAP_START_MB))
profStep(0, None)
if nxt is not None:
    print(f"memory at {heapMB():.0f} MB: going on in a fresh Pyodide at step {nxt}")
nxt
`) ?? null;
    }
    py.runPython(`print(f"converted in ${Math.round((Date.now() - t) / 1000)} s (Pyodide, numpy {__import__('numpy').__version__}, "
      f"Pillow {__import__('PIL').__version__})")`);
  } catch (e) {  // the Python traceback only (the error object also carries Pyodide's minified source)
    console.error(String(e.message || e).slice(-4000));
    process.exitCode = 1;
  }
} finally {
  if (profOut) fs.writeFileSync(profOut, "seg\ti\tstep\tseconds\theap_mb\n" +
    rows.map((r) => `${r.seg}\t${r.i}\t${r.name}\t${r.s.toFixed(2)}\t${r.heap.toFixed(0)}`).join("\n") + "\n");
  fs.rmSync(tmp, { recursive: true, force: true });
}
