// Dark Souls PS2 ISO builder page (tools/pages/index.html). Reads build.json (tools/pages/build.py), checks the
// player's game folder in the browser (SHA-256 of tools/verify_game.py KEY_FILES via WebCrypto; nothing is sent
// anywhere) and writes the one-line command that runs build-iso.sh on their PC.
"use strict";
const $ = (id) => document.getElementById(id);
let BUILD = null;
let OS = /Win/.test(navigator.platform) ? "windows" : /Mac/.test(navigator.platform) ? "mac" : "linux";
let checkedName = "";
let GAME_FILES = null;  // [{path, file}] of the chosen game folder, for the converter (read in place, never sent)

const REQ = {
  linux: `<ul><li><a href="https://docs.docker.com/engine/install/">Docker</a> or Podman (most distros: <code>podman</code> from the package manager).
    Inside a distrobox or toolbox, run the command on the host, or use <i>My own ps2dev toolchain</i>.</li>
    <li>Leave the game folder empty to let the script find a Steam install (native, Flatpak, Snap and extra Steam libraries).</li></ul>`,
  windows: `<ul><li><a href="https://learn.microsoft.com/windows/wsl/install">WSL 2</a> with Ubuntu, and
    <a href="https://docs.docker.com/desktop/features/wsl/">Docker Desktop with WSL integration</a> turned on.</li>
    <li>Run the command in the Ubuntu terminal. Paste your Windows game path below and it is turned into the WSL path
    (<code>C:\\…</code> becomes <code>/mnt/c/…</code>).</li></ul>`,
  mac: `<ul><li><a href="https://docs.docker.com/desktop/setup/install/mac-install/">Docker Desktop</a> (on Apple Silicon the
    PS2 toolchain runs under emulation, so the first build is slower). Give Docker at least 8 GB of memory in its settings.</li>
    <li>DSR is a Windows game: point the script at a copy of its install folder (for example from a Windows PC or CrossOver).</li></ul>`,
};
const PLACEHOLDER = {
  linux: "Leave empty to auto-detect, or e.g. ~/.local/share/Steam/steamapps/common/DARK SOULS REMASTERED",
  windows: "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DARK SOULS REMASTERED",
  mac: "/path/to/DARK SOULS REMASTERED",
};

function esc(s) { return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c])); }
function shq(s) {  // single-quote for bash; a leading ~/ stays outside the quotes so it still expands
  if (s.startsWith("~/")) return "~/" + shq(s.slice(2));
  return "'" + s.replace(/'/g, "'\\''") + "'";
}
function wslPath(p) {
  const m = /^([A-Za-z]):[\\/](.*)$/.exec(p);
  return m ? "/mnt/" + m[1].toLowerCase() + "/" + m[2].replace(/\\/g, "/") : p.replace(/\\/g, "/");
}

async function loadBuild() {
  try {
    const r = await fetch("build.json", { cache: "no-cache" });
    if (!r.ok) throw new Error(r.status);
    BUILD = await r.json();
  } catch (e) {
    $("build").innerHTML = `<dt>Error</dt><dd class="bad">build.json could not be loaded (${esc(e.message)})</dd>`;
    return;
  }
  const url = `https://github.com/${BUILD.repo}`;
  const when = new Date(BUILD.date);
  $("build").innerHTML =
    `<dt>Source</dt><dd><a href="${url}/tree/${BUILD.commit}">${esc(BUILD.repo)} @ <code>${BUILD.commit.slice(0, 10)}</code></a> (main)</dd>` +
    `<dt>Change</dt><dd>${esc(BUILD.subject)}</dd>` +
    `<dt>Date</dt><dd>${isNaN(when) ? esc(BUILD.date) : when.toLocaleString()}</dd>` +
    `<dt>Game</dt><dd>${BUILD.supported.map((v) => esc(v.name)).join("<br>")}</dd>`;
  $("readme").href = `${url}#readme`;
  render();
}

// --- step 1: hash the key files of the chosen folder, locally ---
async function sha256(file) {
  const d = await crypto.subtle.digest("SHA-256", await file.arrayBuffer());
  return Array.from(new Uint8Array(d), (b) => b.toString(16).padStart(2, "0")).join("");
}

async function fileFromHandle(dir, rel) {
  const parts = rel.split("/");
  try {
    for (const p of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(p);
    return await (await dir.getFileHandle(parts[parts.length - 1])).getFile();
  } catch { return null; }
}

async function check(getFile, name) {
  if (!BUILD) return;
  checkedName = name;
  const rows = $("hashes").querySelector("tbody");
  rows.innerHTML = "";
  $("hashes").hidden = false;
  $("verdict").textContent = "";
  const have = {};
  let missing = 0;
  for (const rel of BUILD.key_files) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td class="s">…</td><td><code>${esc(rel)}</code></td>`;
    rows.appendChild(tr);
    $("pickstatus").textContent = `Hashing ${rel}`;
    const f = await getFile(rel);
    if (!f) { tr.cells[0].innerHTML = `<span class="bad">missing</span>`; missing++; continue; }
    have[rel] = await sha256(f);
    tr.cells[0].innerHTML = `<span class="ok">read</span>`;
  }
  $("pickstatus").textContent = name ? `Folder: ${name}` : "";
  if (!have["DarkSoulsRemastered.exe"]) {
    $("verdict").innerHTML = `<span class="bad">No DarkSoulsRemastered.exe in this folder.</span> Pick the folder that holds it
      (it is called <code>DARK SOULS REMASTERED</code>), not its parent.`;
    return;
  }
  let best = null, bestBad = Infinity;
  for (const v of BUILD.supported) {
    const bad = BUILD.key_files.filter((f) => v.files[f] !== have[f]);
    if (bad.length < bestBad) { best = v; bestBad = bad.length; }
  }
  BUILD.key_files.forEach((f, i) => {
    if (have[f]) rows.rows[i].cells[0].innerHTML = best.files[f] === have[f] ? `<span class="ok">match</span>` : `<span class="warn">differs</span>`;
  });
  VERIFIED = bestBad === 0;
  if (bestBad === 0) {
    $("verdict").innerHTML = `<span class="ok">Supported: ${esc(best.name)}.</span> Go on to step 2.`;
  } else if (missing) {
    $("verdict").innerHTML = `<span class="bad">${missing} file(s) are missing.</span> Is the install complete? In Steam:
      <i>Properties → Installed Files → Verify integrity of game files</i>.`;
  } else {
    $("verdict").innerHTML = `<span class="warn">Unknown version: ${bestBad} file(s) differ from ${esc(best.id)}.</span>
      The converter is tested on the supported version only. Updating or verifying the game in Steam usually fixes this.`;
  }
}

$("pick").addEventListener("click", async () => {
  if (window.showDirectoryPicker) {
    let dir;
    try { dir = await window.showDirectoryPicker({ id: "dsr-game", mode: "read" }); } catch { return; }
    // picked the parent (steamapps/common)? use its DARK SOULS REMASTERED folder
    if (!(await fileFromHandle(dir, "DarkSoulsRemastered.exe"))) {
      try { dir = await dir.getDirectoryHandle("DARK SOULS REMASTERED"); } catch { /* keep: reported as missing */ }
    }
    await check((rel) => fileFromHandle(dir, rel), dir.name);
    $("convstatus").textContent = "Listing the game folder…";
    GAME_FILES = [];
    await walk(dir, "", GAME_FILES);
    gameReady();
  } else {
    $("pickfallback").click();  // Firefox / Safari: a folder input; the browser only lists files until one is read
  }
});
$("pickfallback").addEventListener("change", (ev) => {
  const files = Array.from(ev.target.files);
  if (!files.length) return;
  const byRel = new Map();
  for (const f of files) byRel.set(f.webkitRelativePath.split("/").slice(1).join("/"), f);
  let prefix = "";
  if (!byRel.has("DarkSoulsRemastered.exe") && byRel.has("DARK SOULS REMASTERED/DarkSoulsRemastered.exe")) prefix = "DARK SOULS REMASTERED/";
  check(async (rel) => byRel.get(prefix + rel) || null, files[0].webkitRelativePath.split("/")[0]).then(() => {
    GAME_FILES = [];
    for (const [rel, f] of byRel) if (rel.startsWith(prefix)) GAME_FILES.push({ path: rel.slice(prefix.length), file: f });
    gameReady();
  });
});

async function walk(dir, prefix, out) {
  for await (const [name, h] of dir.entries()) {
    if (h.kind === "directory") await walk(h, prefix + name + "/", out);
    else out.push({ path: prefix + name, file: await h.getFile() });
  }
}

// --- step 2: convert in a worker (worker.js) ---
let VERIFIED = false;
function gameReady() {
  const ok = GAME_FILES && GAME_FILES.some((f) => f.path === "DarkSoulsRemastered.exe");
  $("convert").disabled = !ok || !BUILD || !BUILD.converter;
  $("convstatus").textContent = !BUILD || !BUILD.converter ? "This copy of the page has no converter (build.json)."
    : !ok ? "Pick the folder that holds DarkSoulsRemastered.exe."
    : VERIFIED ? `${GAME_FILES.length} files. Ready.` : `${GAME_FILES.length} files. Unknown game version: the conversion may fail.`;
}

$("convert").addEventListener("click", () => {
  $("convert").disabled = true;
  $("save").hidden = true;
  $("bar").hidden = false;
  $("bar").removeAttribute("value");  // indeterminate until the first step
  $("logbox").hidden = false;
  $("log").textContent = "";
  const t0 = Date.now();
  const log = (s) => {
    const el = $("log");
    const stick = el.scrollTop + el.clientHeight >= el.scrollHeight - 4;
    el.textContent += s + "\n";
    if (stick) el.scrollTop = el.scrollHeight;
  };
  const stopped = (msg) => {
    log("ERROR: " + msg);
    $("convstatus").innerHTML = `<span class="bad">The conversion stopped.</span> The log above says where. The script
      below does the same build on your PC.`;
    $("bar").hidden = true;
    $("convert").disabled = false;
  };
  // The plan's groups (build.json converter.groups: shared steps, one per area / world block, then the world) run in
  // up to LANES workers side by side, each group once its deps are done, the ones with map exports first. A group
  // runs as a chain of workers: one whose memory has grown too far hands its build so far to a fresh one ({pause},
  // worker.js), as WebAssembly memory never shrinks. Each finished group's files ({finished}) go to the groups that
  // need them; the last group (the world) gets every file and writes the disc image.
  const groups = (BUILD.converter.groups || [{ name: null, deps: [], maps: 0 }]).map((g) => ({ ...g, state: "wait" }));
  const last = groups[groups.length - 1];
  const lanes = laneCount();
  const files = new Map();  // path -> ArrayBuffer of every finished group's build files
  // the group that made each file: a group gets its deps' files (their packs and the intermediates later steps still
  // name, convert.py --lean)
  const fileGroup = new Map();
  const depFiles = (g) => [...files.keys()].filter((p) => g.deps.includes(fileGroup.get(p)));
  const workers = new Set();
  let failed = false;
  log(`${lanes} worker(s) side by side, ${groups.length} groups`);
  const carryOf = (paths) => [...paths].map((p) => ({ path: p, data: files.get(p).slice(0) }));
  const fail = (msg) => {
    if (failed) return;
    failed = true;
    for (const w of workers) w.terminate();
    stopped(msg);
  };
  const status = () => {
    const run = groups.filter((g) => g.state === "run").map((g) => g.name).join(", ");
    const done = groups.filter((g) => g.state === "done").length;
    const min = Math.floor((Date.now() - t0) / 60000);
    $("bar").max = groups.length; $("bar").value = done;
    $("convstatus").textContent = `${done} of ${groups.length} parts done; working on ${run || "…"} (${min} min so far)`;
  };
  const segment = (g, start, carry) => {
    const w = new Worker("worker.js");
    workers.add(w);
    const end = () => { w.terminate(); workers.delete(w); };
    w.onmessage = (ev) => {
      const m = ev.data;
      if (m.log !== undefined) log(groups.length > 1 && g.name ? `[${g.name}] ${m.log}` : m.log);
      if (m.progress) status();
      if (m.pause) { end(); segment(g, m.pause.next, m.pause.carry); }
      if (m.finished) {
        end();
        for (const f of m.finished.carry) {
          files.set(f.path, f.data);
          if (!fileGroup.has(f.path)) fileGroup.set(f.path, g.name);  // a dep's file handed back again stays the dep's
        }
        g.state = "done";
        status();
        schedule();
      }
      if (m.done) {
        end();
        const url = URL.createObjectURL(new Blob([m.done], { type: "application/octet-stream" }));
        $("save").href = url;
        $("save").hidden = false;
        $("bar").value = $("bar").max;
        $("convstatus").textContent = `Done in ${Math.round((Date.now() - t0) / 60000)} min: ${(m.done.byteLength / 1048576).toFixed(0)} MB.`;
        $("convert").disabled = false;
      }
      if (m.error) { end(); fail(`${g.name ? g.name + ": " : ""}${m.error}`); }
    };
    w.onerror = (e) => { end(); fail(e.message + " (the converter crashed: out of memory? Try fewer workers.)"); };
    const iso = g === last;
    w.postMessage({ convert: { build: BUILD, base: location.href, files: GAME_FILES, start, carry,
                               select: g.name ? [g.name] : null, iso } }, carry.map((f) => f.data));
  };
  const schedule = () => {
    if (failed) return;
    const running = groups.filter((g) => g.state === "run").length;
    const ready = groups.filter((g) => g.state === "wait" && g.deps.every((d) => groups.find((x) => x.name === d).state === "done"))
      .filter((g) => g !== last || groups.every((x) => x === last || x.state === "done"))  // the image needs every pack
      .sort((a, b) => b.maps - a.maps);
    for (const g of ready.slice(0, Math.max(0, lanes - running))) {
      g.state = "run";
      // its dependencies' files, or every file for the last group (the disc image packs them all)
      segment(g, 0, carryOf(g === last ? files.keys() : depFiles(g)));
    }
    status();
  };
  schedule();
});

// workers side by side: the page's choice, or from the device's memory (navigator.deviceMemory, GB, Chrome / Edge:
// rounded down and capped at 8; unknown elsewhere: 8 assumed) and cores. Each worker peaks around 2-2.5 GB
// (convert-node.mjs --prof, 2026-11-02: heap 1956 MB at most, plus its share of the page's copies).
function laneCount() {
  const v = $("lanes").value;
  if (v !== "auto") return Number(v);
  const mem = navigator.deviceMemory || 8, cores = navigator.hardwareConcurrency || 4;
  return Math.max(1, Math.min(3, Math.floor(mem / 3), Math.floor(cores / 2)));
}

// --- step 2: the command ---
function render() {
  for (const b of $("ostabs").querySelectorAll("button")) b.setAttribute("aria-selected", String(b.dataset.os === OS));
  $("req").innerHTML = REQ[OS];
  $("game").placeholder = PLACEHOLDER[OS];
  $("gamehint").textContent = OS === "linux" ? "(optional)" : "";
  $("termhint").textContent = OS === "windows" ? " (the Ubuntu / WSL terminal, in the folder where the ISO should go)"
    : " (in the folder where the ISO should go)";
  if (!BUILD) return;
  const script = new URL("build-iso.sh", location.href).href;
  let game = $("game").value.trim().replace(/^["']|["']$/g, "");
  if (OS === "windows") game = wslPath(game);
  const args = [];
  if (game) args.push("--game " + shq(game));
  if ($("engine").value !== "auto") args.push("--engine " + $("engine").value);
  const lines = [`curl -fsSLo build-iso.sh ${script}`, ["bash build-iso.sh", ...args].join(" ")];
  $("cmd").textContent = lines.join("\n");
}

$("ostabs").addEventListener("click", (ev) => {
  const b = ev.target.closest("button[data-os]");
  if (b) { OS = b.dataset.os; render(); }
});
$("game").addEventListener("input", render);
$("engine").addEventListener("change", render);
$("copy").addEventListener("click", async () => {
  try { await navigator.clipboard.writeText($("cmd").textContent); $("copy").textContent = "Copied"; }
  catch { $("copy").textContent = "Select it"; }
  setTimeout(() => ($("copy").textContent = "Copy"), 1500);
});

render();
loadBuild();
