// Profile the in-browser converter under Node the way tools/pages/worker.js runs it: steps in one Pyodide until its
// WebAssembly memory passes RESTART_MB, then on in a fresh Pyodide with the build so far (worker.js's pause / carry).
// Prints and writes per step: seconds and the wasm heap (HEAP8.length, which only grows) after it, so the heap's
// growth over a step is that step's high-water mark above everything before it in the same Pyodide.
//
//   node tools/pages/profile-node.mjs <game dir> <build dir> [--wasm DIR] [--restart-mb N] [--map-start-mb N] [--old-rule] [--out F.tsv]
//        [--only STEP...]
//
// Restarts follow worker.js: before a map export unless the heap is <= --map-start-mb (400), and past --restart-mb.
// --old-rule: only past --restart-mb (the rule before 2026-11-02).
import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";

if (!process.execArgv.includes("--disable-wasm-trap-handler")) {
  const r = spawnSync(process.execPath, ["--disable-wasm-trap-handler", ...process.execArgv, ...process.argv.slice(1)],
    { stdio: "inherit" });
  process.exit(r.status ?? 1);
}
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..", "..");
const args = process.argv.slice(2);
const opt = (k) => { const i = args.indexOf(k); if (i < 0) return null; const v = args[i + 1]; args.splice(i, 2); return v; };
const flag = (k) => { const i = args.indexOf(k); if (i < 0) return false; args.splice(i, 1); return true; };
let wasm = opt("--wasm");
const restartMB = Number(opt("--restart-mb") || 1536);
const outTsv = opt("--out");
const oldRule = flag("--old-rule");  // restart only past RESTART_MB (worker.js before the map-export rule)
const mapStartMB = Number(opt("--map-start-mb") || 400);
const onlyAt = args.indexOf("--only");
const only = onlyAt >= 0 ? args.splice(onlyAt).slice(1) : null;
if (args.length !== 2) {
  console.error("usage: node tools/pages/profile-node.mjs <game dir> <build dir> [--wasm DIR] [--restart-mb N] [--map-start-mb N] [--old-rule] [--out F.tsv]");
  process.exit(2);
}
const game = path.resolve(args[0]), build = path.resolve(args[1]);
const version = /^PYODIDE = "([^"]+)"/m.exec(fs.readFileSync(path.join(ROOT, "tools/pages/build.py"), "utf8"))[1];
const cache = path.join(ROOT, "build", "cache", "pyodide-node", version);
const mod = path.join(cache, "node_modules", "pyodide", "pyodide.mjs");
if (!fs.existsSync(mod)) {
  fs.mkdirSync(cache, { recursive: true });
  execFileSync("npm", ["install", "--prefix", cache, "--no-save", `pyodide@${version}`], { stdio: "inherit" });
}
const { loadPyodide } = await import(pathToFileURL(mod).href);
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "profile-node-"));
const rows = [];
try {
  if (!wasm) {
    wasm = path.join(tmp, "wasm");
    execFileSync(path.join(ROOT, "tools/native/build-wasm.sh"), [wasm], { stdio: "inherit" });
  }
  const src = path.join(tmp, "src");
  const files = execFileSync("git", ["ls-files", "tools", "ps2/proto/Makefile"], { cwd: ROOT, encoding: "utf8" })
    .split("\n").filter((f) => f === "ps2/proto/Makefile" || (f.split("/").length === 2 && f.endsWith(".py")) ||
      (f.startsWith("tools/native/") && /\.(c|h)$/.test(f)));
  for (const f of files) {  // the working tree's copies (uncommitted edits included)
    fs.mkdirSync(path.dirname(path.join(src, f)), { recursive: true });
    fs.copyFileSync(path.join(ROOT, f), path.join(src, f));
  }
  const later = new Date(Date.now() + 3600e3);
  for (const so of fs.readdirSync(wasm).filter((f) => f.endsWith(".so"))) {
    fs.copyFileSync(path.join(wasm, so), path.join(src, "tools/native", so));
    fs.utimesSync(path.join(src, "tools/native", so), later, later);
  }
  fs.mkdirSync(build, { recursive: true });
  let start = 0, seg = 0;
  const t0 = Date.now();
  while (start !== null) {
    const py = await loadPyodide({ packageCacheDir: path.join(cache, "packages") });
    await py.loadPackage(["numpy", "pillow"], { messageCallback: () => {} });
    for (const [dir, at] of [[src, "/src"], [game, "/game"], [build, "/build"]]) {
      py.FS.mkdirTree(at);
      py.FS.mount(py.FS.filesystems.NODEFS, { root: dir }, at);
    }
    const heap = () => py._module.HEAP8.length / 1048576;
    const segNo = seg++;
    let last = null;
    globalThis.profStep = (i, n, name) => {
      const now = Date.now();
      if (last) {
        const r = { seg: segNo, i: last.i, name: last.name, s: (now - last.t) / 1000, heap: heap() };
        rows.push(r);
        console.log(`  [prof] ${r.s.toFixed(1)} s  heap ${r.heap.toFixed(0)} MB  ${r.name}`);
      }
      last = name === null ? null : { i, name, t: now };
    };
    globalThis.heapMB = heap;  // export_map.rss() prints it per stage
    py.globals.set("START", start);
    py.globals.set("RESTART_MB", restartMB);
    py.globals.set("FRESH_EACH", !oldRule);
    py.globals.set("MAP_START_MB", mapStartMB);
    py.globals.set("ONLY", only ? py.toPy(only) : null);
    const nxt = py.runPython(`
import os, sys, time
os.makedirs("/proc/self", exist_ok=True)
open("/proc/self/status", "w").write("VmRSS:\\t0 kB\\nVmHWM:\\t0 kB\\n")
sys.path.insert(0, "/src/tools")
os.environ["SIMPLIFY_PROCS"] = "1"
os.environ["SIMPLIFY_CACHE"] = "0"
os.environ["TEX_CACHE"] = "0"
os.environ["TMPDIR"] = "/tmp"
import convert
from js import profStep, heapMB
_names = {}
def _prog(i, n, name):
    _names["next"] = name
    profStep(i, n, name)
def _pause(nxt):
    if FRESH_EACH:  # worker.js's rule
        return heapMB() > RESTART_MB or (convert.is_map_export(nxt) and heapMB() > MAP_START_MB)
    return heapMB() > RESTART_MB
times, nxt = convert.run_part("/game", "/build", only=ONLY, progress=_prog, lean=True, start=START, pause=_pause)
profStep(0, 0, None)
nxt
`);
    console.log(`[prof] segment ${segNo} ended at heap ${heap().toFixed(0)} MB; next step ${nxt}`);
    start = nxt === undefined ? null : nxt;
  }
  console.log(`[prof] total ${((Date.now() - t0) / 1000).toFixed(0)} s in ${seg} Pyodide instances`);
} catch (e) {
  console.error(String(e.message || e).slice(-4000));
  process.exitCode = 1;
} finally {
  if (outTsv) fs.writeFileSync(outTsv, "seg\ti\tstep\tseconds\theap_mb\n" +
    rows.map((r) => `${r.seg}\t${r.i}\t${r.name}\t${r.s.toFixed(2)}\t${r.heap.toFixed(0)}`).join("\n") + "\n");
  fs.rmSync(tmp, { recursive: true, force: true });
}
