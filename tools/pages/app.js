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
  // one worker per stretch of steps: a worker whose memory has grown too far hands the build so far to a fresh one
  // ({pause}, worker.js RESTART_MB), as WebAssembly memory never shrinks
  const segment = (start, carry) => {
    const w = new Worker("worker.js");
    w.onmessage = (ev) => {
      const m = ev.data;
      if (m.log !== undefined) log(m.log);
      if (m.progress) {
        const [i, n, name] = m.progress;
        $("bar").max = n; $("bar").value = i;
        const min = Math.floor((Date.now() - t0) / 60000);
        $("convstatus").textContent = `Step ${i + 1} of ${n}: ${name} (${min} min so far)`;
      }
      if (m.pause) {
        w.terminate();
        segment(m.pause.next, m.pause.carry);
      }
      if (m.done) {
        const url = URL.createObjectURL(new Blob([m.done], { type: "application/octet-stream" }));
        $("save").href = url;
        $("save").hidden = false;
        $("bar").value = $("bar").max;
        $("convstatus").textContent = `Done in ${Math.round((Date.now() - t0) / 60000)} min: ${(m.done.byteLength / 1048576).toFixed(0)} MB.`;
        w.terminate();
        $("convert").disabled = false;
      }
      if (m.error) {
        w.terminate();
        stopped(m.error);
      }
    };
    w.onerror = (e) => { w.terminate(); stopped(e.message + " (the converter crashed: out of memory?)"); };
    w.postMessage({ convert: { build: BUILD, base: location.href, files: GAME_FILES, start, carry } },
                  carry.map((f) => f.data));
  };
  segment(0, []);
});

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
