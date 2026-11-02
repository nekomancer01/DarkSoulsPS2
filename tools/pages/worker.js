// In-browser converter (tools/pages): Pyodide runs the port's own exporters (tools/convert.py, the Makefile's steps)
// on the player's game folder, then tools/iso9660.py packs the game-free ELF from CI and the packs into a disc image.
// Everything stays in this worker's memory: the game files are read in place (WORKERFS over the File objects the page
// passes in), nothing is sent anywhere. In: {convert: {build, base, files, start, carry, select, iso}} (select: the
// plan groups this worker runs, convert.groups(); null = all; iso: write the disc image when they are done). Out:
// {log}, {progress: [i, n, name]}, {pause: {next, carry}} (go on in a new worker: RESTART_MB / MAP_START_MB),
// {finished: {carry}} (the groups are done; carry = this worker's build files), {done: ArrayBuffer} (the disc image),
// {error}.
"use strict";
let py = null;

function post(m, transfer) { self.postMessage(m, transfer || []); }

async function setup(build, base) {
  const pyodideURL = build.pyodide_url;  // e.g. https://cdn.jsdelivr.net/pyodide/v0.27.7/full/
  importScripts(pyodideURL + "pyodide.js");
  post({ log: `Loading Python (Pyodide ${build.pyodide_version})…` });
  py = await loadPyodide({ indexURL: pyodideURL });
  py.setStdout({ batched: (s) => post({ log: s }) });
  py.setStderr({ batched: (s) => post({ log: s }) });
  await py.loadPackage(["numpy", "pillow"], { messageCallback: () => {} });
  post({ log: "Loading the converter (source " + build.commit.slice(0, 10) + ")…" });
  // the port's tools, the Makefile (for its knobs) and the C helpers built for WebAssembly, at the page's commit
  const src = await (await fetch(new URL("convert-src.zip", base))).arrayBuffer();
  py.unpackArchive(src, "zip", { extractDir: "/src" });
  // export_map.py logs its memory from /proc/self/status, which a browser has none of
  py.runPython(`
import os
os.makedirs("/proc/self", exist_ok=True)
open("/proc/self/status", "w").write("VmRSS:\\t0 kB\\nVmHWM:\\t0 kB\\n")
`);
  // the .so files must look newer than their .c sources, or the tools would try to compile them with gcc
  py.runPython(`
import os, time
now = time.time() + 60
for f in os.listdir("/src/tools/native"):
    if f.endswith(".so"):
        os.utime(os.path.join("/src/tools/native", f), (now, now))
`);
}

function mountGame(files) {
  // files: [{path: "chr/c0000.chrbnd.dcx", file: File}] relative to the game folder
  py.FS.mkdirTree("/game");
  py.FS.mount(py.FS.filesystems.WORKERFS, { blobs: files.map((f) => ({ name: f.path, data: f.file })) }, "/game");
}

// WebAssembly memory only grows: one map export takes it up by ~2.5 GB (m10_01_00_00_w: 770 -> 3232 MB, measured
// 2026-11-02 under Node), so a map export that starts on a heap of more than ~1 GB runs into the 4 GB ceiling (wasm32)
// mid-step. So the worker hands /build (~100 MB, see convert.py --lean) back to the page, which goes on in a fresh
// worker (convert.run_part), before every map export unless the heap is still near a fresh Pyodide's
// (MAP_START_MB), and after any step that leaves more than RESTART_MB.
const RESTART_MB = 1536;
const MAP_START_MB = 400;

function buildFiles(dir, out) {
  // every file under dir in this Pyodide's file system -> [{path, data}] (data: ArrayBuffer, transferable)
  for (const name of py.FS.readdir(dir)) {
    if (name === "." || name === "..") continue;
    const p = dir + "/" + name;
    if (py.FS.isDir(py.FS.stat(p).mode)) buildFiles(p, out);
    else out.push({ path: p, data: py.FS.readFile(p).buffer });
  }
  return out;
}

async function convert(msg) {
  const base = msg.base;
  if (!py) await setup(msg.build, base);
  mountGame(msg.files);
  for (const f of msg.carry || []) {  // the build so far, from the previous worker
    py.FS.mkdirTree(f.path.slice(0, f.path.lastIndexOf("/")));
    py.FS.writeFile(f.path, new Uint8Array(f.data));
  }
  py.FS.mkdirTree("/build");
  const elf = new Uint8Array(await (await fetch(new URL("proto.elf", base))).arrayBuffer());
  py.FS.writeFile("/proto.elf", elf);
  self.convertProgress = (i, n, name) => post({ progress: [i, n, name] });
  self.heapMB = () => py._module.HEAP8.length / 1048576;
  py.globals.set("START", msg.start || 0);
  py.globals.set("SELECT", msg.select ? py.toPy(msg.select) : null);
  py.globals.set("RESTART_MB", RESTART_MB);
  py.globals.set("MAP_START_MB", MAP_START_MB);
  const next = py.runPython(`
import os, sys, time
sys.path.insert(0, "/src/tools")
os.environ["SIMPLIFY_PROCS"] = "1"   # no processes in a browser: simplify in this one
os.environ["SIMPLIFY_CACHE"] = "0"   # its cache would only fill memory
os.environ["TEX_CACHE"] = "0"        # texmap.encode's cache too (a compressed copy of every texture)
os.environ["TMPDIR"] = "/tmp"
import convert
from js import convertProgress, heapMB
t = time.time()
# lean: intermediates leave memory once the steps after them no longer need them
times, nxt = convert.run_part("/game", "/build", progress=lambda i, n, name: convertProgress(i, n, name), lean=True,
                              start=START, select=SELECT, pause=lambda nxt: heapMB() > RESTART_MB or
                              (convert.is_map_export(nxt) and heapMB() > MAP_START_MB))
if nxt is not None:
    print(f"memory at {heapMB():.0f} MB after {time.time() - t:.0f} s: going on in a fresh Python")
nxt
`);
  if (next !== undefined && next !== null) {
    const carry = buildFiles("/build", []);
    post({ pause: { next, carry } }, carry.map((f) => f.data));
    return;
  }
  if (msg.iso === false) {  // one group of a parallel run: its files go to the page, which merges them
    const carry = buildFiles("/build", []);
    post({ finished: { carry } }, carry.map((f) => f.data));
    return;
  }
  py.runPython(`
import iso9660, shutil
size = iso9660.build("/proto.elf", "/build/data", "/DarkSoulsPS2.iso")
print(f"DarkSoulsPS2.iso: {size / 1048576:.1f} MB")
shutil.rmtree("/build")  # the packs are in the image now: free them before the page gets its copy of it
`);
  const iso = py.FS.readFile("/DarkSoulsPS2.iso");
  py.FS.unlink("/DarkSoulsPS2.iso");
  post({ done: iso.buffer }, [iso.buffer]);
}

self.onmessage = async (ev) => {
  try {
    if (ev.data.convert) await convert(ev.data.convert);
  } catch (e) {
    post({ error: String(e && e.message || e) });
  }
};
