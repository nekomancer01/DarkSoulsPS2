// In-browser converter (tools/pages): Pyodide runs the port's own exporters (tools/convert.py, the Makefile's steps)
// on the player's game folder, then tools/iso9660.py packs the game-free ELF from CI and the packs into a disc image.
// Everything stays in this worker's memory: the game files are read in place (WORKERFS over the File objects the page
// passes in), nothing is sent anywhere. Messages out: {log}, {progress: [i, n, name]}, {done: ArrayBuffer}, {error}.
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

async function convert(msg) {
  const base = msg.base;
  if (!py) await setup(msg.build, base);
  mountGame(msg.files);
  const elf = new Uint8Array(await (await fetch(new URL("proto.elf", base))).arrayBuffer());
  py.FS.writeFile("/proto.elf", elf);
  self.convertProgress = (i, n, name) => post({ progress: [i, n, name] });
  py.runPython(`
import os, sys, time
sys.path.insert(0, "/src/tools")
os.environ["SIMPLIFY_PROCS"] = "1"   # no processes in a browser: simplify in this one
os.environ["SIMPLIFY_CACHE"] = "0"   # its cache would only fill memory
os.environ["TEX_CACHE"] = "0"        # texmap.encode's cache too (a compressed copy of every texture)
os.environ["TMPDIR"] = "/tmp"
import convert, iso9660
from js import convertProgress
t = time.time()
times = convert.run("/game", "/build", progress=lambda i, n, name: convertProgress(i, n, name))
print(f"converted in {time.time() - t:.0f} s; writing the disc image")
size = iso9660.build("/proto.elf", "/build/data", "/DarkSoulsPS2.iso")
print(f"DarkSoulsPS2.iso: {size / 1048576:.1f} MB")
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
