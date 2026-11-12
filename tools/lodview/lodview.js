// LOD budget viewer front end (served by tools/lodview.py). Data: /api/meta, /api/mesh, POST /api/budget.
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

const $ = (id) => document.getElementById(id);
const LEVELS = ['near', 'coarse', 'kept', 'backdrop'];
const LEVEL_COL = { near: '#4fbf7a', coarse: '#6cb4ff', kept: '#e0a83a', backdrop: '#b48cf0' };
const OFF = -1;  // near level picked "off": the cell draws its coarse level from 0 m (MS_ONE_LOD)

let meta, baseline = null, last = null;
const st = {
  glob: { near: 0, coarse: 0, kept: 0, backdrop: 0 },  // index into meta.ratios, or OFF (near only)
  ovr: new Map(),                                        // "b:c" -> {level: r}
  radii: {},
  mode: 'runtime', colour: 'texture', wire: false, spots: true, sky: false, rings: true,
  sel: null,
};

// ---------- three.js scene ----------
const view = $('view');
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(2, window.devicePixelRatio));
renderer.outputColorSpace = THREE.LinearSRGBColorSpace;
view.prepend(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x15171a);
const camera = new THREE.PerspectiveCamera(50, 1, 0.3, 6000);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
scene.add(new THREE.AmbientLight(0xffffff, 0.75));
const sun = new THREE.DirectionalLight(0xffffff, 0.55);
sun.position.set(0.4, 1, 0.3);
scene.add(sun);
const world = new THREE.Group();
scene.add(world);
const rings = new THREE.Group();
scene.add(rings);
const selBox = new THREE.Box3Helper(new THREE.Box3(), 0xffffff);
selBox.visible = false;
scene.add(selBox);
const focusDot = new THREE.Mesh(new THREE.SphereGeometry(0.4, 12, 8), new THREE.MeshBasicMaterial({ color: 0xffffff }));
scene.add(focusDot);

const mats = {
  texture: new THREE.MeshLambertMaterial({ vertexColors: true, flatShading: true, side: THREE.DoubleSide }),
  flat: (c) => new THREE.MeshLambertMaterial({ color: c, flatShading: true, side: THREE.DoubleSide }),
};
const flatMats = new Map();
function flatMat(c) {
  if (!flatMats.has(c)) flatMats.set(c, mats.flat(c));
  return flatMats.get(c);
}

function resize() {
  const r = view.getBoundingClientRect();
  renderer.setSize(r.width, r.height, false);
  camera.aspect = r.width / Math.max(1, r.height);
  camera.updateProjectionMatrix();
}
window.addEventListener('resize', resize);

// ---------- data ----------
const geoCache = new Map();  // "b:lev:r" -> Promise<Map(cell -> BufferGeometry)>
function fetchMesh(b, lev, r) {
  const key = `${b}:${lev}:${r}`;
  if (!geoCache.has(key)) {
    geoCache.set(key, fetch(`/api/mesh?block=${b}&level=${lev}&r=${r}`).then((x) => x.arrayBuffer()).then((buf) => {
      const dv = new DataView(buf);
      const out = new Map();
      let o = 4;
      const n = dv.getUint32(0, true);
      for (let i = 0; i < n; i++) {
        const c = dv.getInt32(o, true), nv = dv.getUint32(o + 4, true), ni = dv.getUint32(o + 8, true);
        o += 12;
        const pos = new Float32Array(buf.slice(o, o + nv * 12)); o += nv * 12;
        const col = new Uint8Array(buf.slice(o, o + nv * 4)); o += nv * 4;
        const idx = new Uint32Array(buf.slice(o, o + ni * 4)); o += ni * 4;
        const g = new THREE.BufferGeometry();
        g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
        g.setAttribute('color', new THREE.BufferAttribute(col, 4, true));
        g.setIndex(new THREE.BufferAttribute(idx, 1));
        g.computeBoundingSphere();
        out.set(c, g);
      }
      return out;
    }));
  }
  return geoCache.get(key);
}

const objs = [];  // objs[b][lev] = Map(cell -> Mesh)
const skyObjs = [];
const spotPts = [];

function pick(b, c, lev) {  // ratio index in force for a cell and level
  const o = st.ovr.get(`${b}:${c}`);
  return o && o[lev] !== undefined ? o[lev] : st.glob[lev];
}

function factors() {
  const f = {};
  meta.blocks.forEach((blk, b) => {
    f[blk.name] = blk.cells.map((cell, c) => LEVELS.map((lev, l) => {
      const r = pick(b, c, lev);
      return r === OFF ? 0 : cell.achieved[l][r];
    }));
  });
  return f;
}

async function applyGeometry() {
  const jobs = [];
  meta.blocks.forEach((blk, b) => LEVELS.forEach((lev) => {
    const want = new Map();
    blk.cells.forEach((_, c) => { const r = pick(b, c, lev); want.set(c, r === OFF ? 0 : r); });
    const rs = new Set(want.values());
    jobs.push(Promise.all([...rs].map((r) => fetchMesh(b, lev, r).then((m) => [r, m]))).then((sets) => {
      const byR = new Map(sets);
      for (const [c, mesh] of objs[b][lev]) {
        const g = byR.get(want.get(c)).get(c);
        if (g && mesh.geometry !== g) mesh.geometry = g;
      }
    }));
  }));
  await Promise.all(jobs);
  updateVisibility();
}

// ---------- geometry helpers ----------
function boxDist(p, lo, hi) {
  const dx = Math.max(0, lo[0] - p.x, p.x - hi[0]);
  const dy = Math.max(0, lo[1] - p.y, p.y - hi[1]);
  const dz = Math.max(0, lo[2] - p.z, p.z - hi[2]);
  return Math.hypot(dx, dy, dz);
}
function nearDist(p, cell) {  // mapstream.h cell_near_d2: near box clipped to the cell's 32 m cube (NEAR_CUBE)
  const nb = cell.near_box, k = cell.key;
  if (!meta.near_cube) return boxDist(p, nb.slice(0, 3), nb.slice(3));
  const lo = [0, 1, 2].map((a) => Math.max(nb[a], k[a] * 32));
  const hi = [0, 1, 2].map((a) => Math.max(Math.min(nb[a + 3], k[a] * 32 + 32), lo[a]));
  return boxDist(p, lo, hi);
}

function ratioCol(r) {  // LOD heat: full detail grey, lower ratios warmer
  return r === OFF ? '#e5604f' : ['#8a929b', '#9fc3e6', '#6cb4ff', '#4fbf7a', '#e0a83a', '#e5604f'][r] || '#e5604f';
}

function updateVisibility() {
  const T = controls.target;
  let tris = 0;
  const bandTris = { near: 0, coarse: 0, kept: 0, backdrop: 0 };
  meta.blocks.forEach((blk, b) => {
    blk.cells.forEach((cell, c) => {
      const d = boxDist(T, cell.box.slice(0, 3), cell.box.slice(3));
      const nearOn = pick(b, c, 'near') !== OFF && cell.tris[0] > 0 && nearDist(T, cell) < st.radii.NEAR_R;
      LEVELS.forEach((lev, l) => {
        const m = objs[b][lev].get(c);
        if (!m) return;
        let vis;
        if (st.mode === 'runtime') {
          vis = lev === 'near' ? nearOn : lev === 'coarse' ? !nearOn : true;
          if (lev !== 'backdrop' && d > st.radii.HARD_R && st.hideFar) vis = false;
        } else vis = st.mode === lev;
        m.visible = vis;
        const r = pick(b, c, lev);
        m.material = st.colour === 'texture' ? mats.texture
          : st.colour === 'level' ? flatMat(LEVEL_COL[lev]) : flatMat(ratioCol(r));
        m.material.wireframe = st.wire;
        if (vis && m.geometry.index) { const n = m.geometry.index.count / 3; tris += n; bandTris[lev] += n; }
      });
    });
    if (skyObjs[b]) skyObjs[b].visible = st.sky;
    if (spotPts[b]) spotPts[b].visible = st.spots;
  });
  mats.texture.wireframe = st.wire;
  for (const m of flatMats.values()) m.wireframe = st.wire;
  focusDot.position.copy(T);
  drawRings();
  updateHud(tris, bandTris);
}

function drawRings() {
  rings.clear();
  if (!st.rings) return;
  const T = controls.target;
  [['NEAR_R', 0x4fbf7a], ['HARD_R', 0x6cb4ff], ['PIN_R', 0xb48cf0]].forEach(([k, col]) => {
    const r = st.radii[k], pts = [];
    for (let i = 0; i <= 96; i++) { const a = i / 96 * Math.PI * 2; pts.push(new THREE.Vector3(T.x + r * Math.cos(a), T.y + 0.2, T.z + r * Math.sin(a))); }
    rings.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), new THREE.LineBasicMaterial({ color: col })));
  });
}

// ---------- spots ----------
function spotColours() {
  if (!last) return;
  let o = 0;
  meta.blocks.forEach((blk, b) => {
    const n = blk.spots.length / 3, col = spotPts[b].geometry.attributes.color;
    for (let i = 0; i < n; i++) {
      const v = last.spot_total[o + i] / last.room_kb;
      const c = new THREE.Color(v > 1 ? 0xe5604f : v > 0.85 ? 0xe0a83a : 0x4fbf7a);
      col.setXYZ(i, c.r, c.g, c.b);
    }
    col.needsUpdate = true;
    o += n;
  });
}

function nearestSpot() {
  const T = controls.target;
  let best = -1, bd = Infinity, o = 0, bb = 0;
  meta.blocks.forEach((blk, b) => {
    const s = blk.spots;
    for (let i = 0; i < s.length; i += 3) {
      const d = (s[i] - T.x) ** 2 + (s[i + 1] - T.y) ** 2 + (s[i + 2] - T.z) ** 2;
      if (d < bd) { bd = d; best = o + i / 3; bb = b; }
    }
    o += s.length / 3;
  });
  return best < 0 ? null : { i: best, b: bb, d: Math.sqrt(bd) };
}

function updateSpot() {
  const s = nearestSpot();
  if (!s || !last) { $('spot').textContent = 'No spot yet.'; return; }
  const L = last, i = s.i, room = L.room_kb;
  const tot = L.spot_total[i], hard = L.spot_hard[i];
  const cls = (v) => v > room ? 'bad' : v > room * 0.85 ? 'warn' : 'ok';
  const b0 = baseline ? baseline.spot_total[i] : null;
  $('spot').innerHTML = `<div class="sub">Nearest navmesh spot of ${meta.blocks[s.b].name}, ${s.d.toFixed(1)} m from the focus point.</div>
  <table>
  <tr><td>Arena want (all tiers)</td><td class="n ${cls(tot)}">${tot} KB</td></tr>
  <tr><td>Hard tier${L.spot_gated[i] ? ' (gated)' : ''}</td><td class="n ${cls(hard)}">${hard} KB</td></tr>
  <tr><td><span class="sw" style="background:${LEVEL_COL.near}"></span>Near within ${st.radii.NEAR_R} m</td><td class="n">${L.spot_near[i]} KB</td></tr>
  <tr><td><span class="sw" style="background:${LEVEL_COL.coarse}"></span>Coarse to ${st.radii.PIN_R} m</td><td class="n">${L.spot_coarse[i]} KB</td></tr>
  <tr><td><span class="sw" style="background:${LEVEL_COL.kept}"></span>Kept</td><td class="n">${L.spot_kept[i]} KB</td></tr>
  <tr><td><span class="sw" style="background:${LEVEL_COL.backdrop}"></span>Backdrop + sky</td><td class="n">${L.spot_far[i]} KB</td></tr>
  <tr><td>Room</td><td class="n">${room} KB</td></tr>
  ${b0 !== null && b0 !== tot ? `<tr><td class="delta">vs. full detail</td><td class="n delta">${tot - b0 > 0 ? '+' : ''}${tot - b0} KB</td></tr>` : ''}
  </table>`;
}

function updateHud(tris, band) {
  const T = controls.target;
  $('hud').innerHTML = `<b>Focus</b> ${T.x.toFixed(1)}, ${T.y.toFixed(1)}, ${T.z.toFixed(1)}<br>
  <b>Triangles shown</b> ${Math.round(tris).toLocaleString()}<br>
  <span class="sw" style="background:${LEVEL_COL.near}"></span>near ${Math.round(band.near).toLocaleString()}
  <span class="sw" style="background:${LEVEL_COL.coarse};margin-left:8px"></span>coarse ${Math.round(band.coarse).toLocaleString()}<br>
  <span class="sw" style="background:${LEVEL_COL.kept}"></span>kept ${Math.round(band.kept).toLocaleString()}
  <span class="sw" style="background:${LEVEL_COL.backdrop};margin-left:8px"></span>backdrop ${Math.round(band.backdrop).toLocaleString()}`;
}

// ---------- budget ----------
let budgetTimer = null, budgetSeq = 0;
function requestBudget() {
  clearTimeout(budgetTimer);
  budgetTimer = setTimeout(runBudget, 250);
}
async function runBudget() {
  const seq = ++budgetSeq;
  $('busy').hidden = false;
  const radii = { NEAR_R: st.radii.NEAR_R, HARD_NEAR_R: st.radii.NEAR_R, HARD_R: st.radii.HARD_R, PIN_R: st.radii.PIN_R };
  const res = await fetch('/api/budget', { method: 'POST', body: JSON.stringify({ factors: factors(), radii }) }).then((r) => r.json());
  if (seq !== budgetSeq) return;
  $('busy').hidden = true;
  last = res;
  if (!baseline) baseline = res;
  renderBudget();
  spotColours();
  updateSpot();
  $('log').textContent = res.log.join('\n') + `\n(${res.ms} ms)`;
}

function meter(label, v, max, base, { unit = 'KB', limit = max, invert = false, note = '' } = {}) {
  const pctv = Math.min(100, v / max * 100);
  const cls = v > limit ? 'bad' : v > limit * 0.9 ? 'warn' : 'ok';
  const col = { ok: 'var(--ok)', warn: 'var(--warn)', bad: 'var(--bad)' }[cls];
  const d = base !== null && base !== undefined && base !== v ? ` <span class="delta">(${v - base > 0 ? '+' : ''}${Math.round((v - base) * 100) / 100})</span>` : '';
  const bm = base !== null && base !== undefined ? `<div class="base" style="left:${Math.min(100, base / max * 100)}%"></div>` : '';
  return `<div class="meter"><div class="lbl"><span>${label}</span><span class="${cls}">${v} ${unit}${d}</span></div>
    <div class="track"><div class="fill" style="width:${pctv}%;background:${col}"></div>${bm}</div>${note ? `<div class="sub">${note}</div>` : ''}</div>`;
}

function renderBudget() {
  const L = last, B = baseline, p = L.pools || {};
  const room = L.room_kb, top = Math.max(room * 1.25, L.total.max);
  let h = `<table>
    <tr><td>Arena (world.c WORLD_ARENA_KB)</td><td class="n">${L.arena_kb} KB</td></tr>
    <tr><td>Structural pool</td><td class="n">${p.structural_kb ?? '–'} KB</td></tr>
    <tr><td>Misc pool</td><td class="n">${p.misc_kb ?? '–'} KB</td></tr>
    <tr><td><b>Map visuals room</b> (x ${L.fill_pct} %)</td><td class="n"><b>${room} KB</b></td></tr></table>`;
  h += meter('Arena want, p90 of spots', L.total.p90, top, B.total.p90, { limit: room });
  h += meter('Arena want, worst spot', L.total.max, top, B.total.max, { limit: room, note: `at ${L.worst.block} (${L.worst.pos.join(', ')})` });
  h += meter('Spots over the room', L.spots_over_room_pct, 100, B.spots_over_room_pct, { unit: '%', limit: 5 });
  h += meter('Hard tier p90 (gated blocks)', L.hard.p90, top, B.hard.p90, { limit: room });
  const g = L.hard.world_over_or_allowance_pct;
  h += meter('Hard tier gate: over or on allowance', g, Math.max(3, g), B.hard.world_over_or_allowance_pct,
    { unit: '%', limit: 1, note: L.strict_fail ? '<span class="bad">--strict would fail the build</span>' : '<span class="ok">passes --strict (at most 1 %)</span>' });
  h += meter(`Near within 30 m, p90 (WS_B0)`, L.near_ws.p90, Math.max(L.near_ws.budget_kb * 1.4, L.near_ws.max), B.near_ws.p90, { limit: L.near_ws.budget_kb, note: `${L.near_ws.over_pct} % of spots over ${L.near_ws.budget_kb} KB` });
  h += meter(`Coarse 16-96 m, p90 (WS_B1)`, L.coarse_ws.p90, Math.max(L.coarse_ws.budget_kb * 1.4, L.coarse_ws.max), B.coarse_ws.p90, { limit: L.coarse_ws.budget_kb, note: `${L.coarse_ws.over_pct} % of spots over ${L.coarse_ws.budget_kb} KB (blocks summed)` });
  h += `<div class="sub">White tick = full detail at the default bands. Bytes of a simplified level are estimated from its triangle count.</div>`;
  $('budget').innerHTML = h;
}

// ---------- panels ----------
function seg(el, items, cur, onPick, ovrSet) {
  el.innerHTML = '';
  items.forEach(([v, label]) => {
    const b = document.createElement('button');
    b.textContent = label;
    if (v === cur) b.classList.add('on');
    if (ovrSet && ovrSet.has(v)) b.classList.add('ovr');
    b.onclick = () => onPick(v);
    el.appendChild(b);
  });
}
const ratioItems = (lev) => (lev === 'near' ? [[OFF, 'off']] : []).concat(meta.ratios.map((r, i) => [i, `${Math.round(r * 100)}%`]));

function levelStats(lev) {
  const l = LEVELS.indexOf(lev);
  let kb = 0, kb0 = 0, tr = 0, tr0 = 0;
  meta.blocks.forEach((blk, b) => blk.cells.forEach((cell, c) => {
    const r = pick(b, c, lev), f = r === OFF ? 0 : cell.achieved[l][r];
    kb0 += cell.kb[l]; kb += cell.kb[l] * f; tr0 += cell.tris[l]; tr += cell.tris[l] * f;
  }));
  return `${Math.round(tr).toLocaleString()} / ${Math.round(tr0).toLocaleString()} tris, ${Math.round(kb)} / ${Math.round(kb0)} KB`;
}

function renderLevels() {
  const el = $('levels');
  el.innerHTML = '';
  LEVELS.forEach((lev) => {
    const d = document.createElement('div');
    d.className = 'lvl';
    d.innerHTML = `<div class="head"><span><span class="sw" style="background:${LEVEL_COL[lev]}"></span>${lev}</span><span>${levelStats(lev)}</span></div><div class="seg"></div>`;
    seg(d.querySelector('.seg'), ratioItems(lev), st.glob[lev], (v) => { st.glob[lev] = v; changed(); });
    el.appendChild(d);
  });
}

function renderCell() {
  const el = $('cell');
  if (!st.sel) { el.textContent = 'Click a cell in the view.'; selBox.visible = false; return; }
  const { b, c } = st.sel, cell = meta.blocks[b].cells[c], key = `${b}:${c}`;
  const o = st.ovr.get(key) || {};
  selBox.box.min.set(...cell.box.slice(0, 3));
  selBox.box.max.set(...cell.box.slice(3));
  selBox.visible = true;
  el.innerHTML = `<div class="sub" style="color:var(--text)">${meta.blocks[b].name}, cell ${cell.key.join(', ')} (${c})</div>`;
  LEVELS.forEach((lev, l) => {
    if (!cell.tris[l]) return;
    const r = pick(b, c, lev), f = r === OFF ? 0 : cell.achieved[l][r];
    const d = document.createElement('div');
    d.className = 'lvl';
    d.innerHTML = `<div class="head"><span><span class="sw" style="background:${LEVEL_COL[lev]}"></span>${lev}${o[lev] !== undefined ? ' (own pick)' : ''}</span>
      <span>${Math.round(cell.tris[l] * f)} / ${cell.tris[l]} tris, ${(cell.kb[l] * f).toFixed(1)} / ${cell.kb[l]} KB</span></div><div class="seg"></div>`;
    seg(d.querySelector('.seg'), [['g', 'global']].concat(ratioItems(lev)), o[lev] === undefined ? 'g' : o[lev], (v) => {
      const cur = { ...(st.ovr.get(key) || {}) };
      if (v === 'g') delete cur[lev]; else cur[lev] = v;
      if (Object.keys(cur).length) st.ovr.set(key, cur); else st.ovr.delete(key);
      changed();
    });
    el.appendChild(d);
  });
}

function renderView() {
  seg($('mode'), [['runtime', 'runtime from focus'], ...LEVELS.map((l) => [l, l])], st.mode, (v) => { st.mode = v; renderView(); updateVisibility(); });
  seg($('colour'), [['texture', 'baked light'], ['level', 'by level'], ['lod', 'by LOD pick']], st.colour, (v) => { st.colour = v; renderView(); updateVisibility(); });
  const lay = [['wire', 'wireframe'], ['spots', 'spot heat'], ['rings', 'band rings'], ['sky', 'sky'], ['hideFar', 'cut at HZN_R']];
  const on = new Set(lay.filter(([k]) => st[k]).map(([k]) => k));
  const el = $('layers');
  el.innerHTML = '';
  lay.forEach(([k, label]) => {
    const b = document.createElement('button');
    b.textContent = label;
    if (on.has(k)) b.classList.add('on');
    b.onclick = () => { st[k] = !st[k]; renderView(); updateVisibility(); };
    el.appendChild(b);
  });
}

function renderRadii() {
  [['near', 'NEAR_R'], ['hard', 'HARD_R'], ['pin', 'PIN_R']].forEach(([id, k]) => {
    $('r_' + id).value = st.radii[k];
    $('v_' + id).textContent = st.radii[k] + ' m';
  });
}
function setupRadii() {
  [['near', 'NEAR_R'], ['hard', 'HARD_R'], ['pin', 'PIN_R']].forEach(([id, k]) => {
    $('r_' + id).oninput = (e) => { st.radii[k] = +e.target.value; renderRadii(); updateVisibility(); requestBudget(); };
  });
  const presets = [
    ['main', { NEAR_R: meta.radii.NEAR_R, HARD_R: meta.radii.HARD_R, PIN_R: meta.radii.PIN_R }, 'main (HZN_R 40)'],
    ['strip', { NEAR_R: meta.radii.NEAR_R, HARD_R: 32, PIN_R: meta.radii.PIN_R }, 'strip branch (HZN_R 32)'],
  ];
  seg($('presets'), presets.map(([k, , l]) => [k, l]), null, (k) => {
    Object.assign(st.radii, presets.find((p) => p[0] === k)[1]);
    renderRadii(); updateVisibility(); requestBudget();
  });
}

function changed() {
  renderLevels();
  renderCell();
  applyGeometry();
  requestBudget();
}

// ---------- interaction ----------
const ray = new THREE.Raycaster();
function hit(ev) {
  const r = renderer.domElement.getBoundingClientRect();
  const m = new THREE.Vector2((ev.clientX - r.left) / r.width * 2 - 1, -(ev.clientY - r.top) / r.height * 2 + 1);
  ray.setFromCamera(m, camera);
  const vis = [];
  world.traverse((o) => { if (o.isMesh && o.visible && o.userData.cell !== undefined) vis.push(o); });
  return ray.intersectObjects(vis, false)[0];
}
let down = null;
renderer.domElement.addEventListener('pointerdown', (e) => { down = [e.clientX, e.clientY]; });
renderer.domElement.addEventListener('pointerup', (e) => {
  if (!down || Math.hypot(e.clientX - down[0], e.clientY - down[1]) > 4 || e.button !== 0) return;
  const h = hit(e);
  if (h) { st.sel = { b: h.object.userData.block, c: h.object.userData.cell }; renderCell(); }
});
renderer.domElement.addEventListener('dblclick', (e) => {
  const h = hit(e);
  if (!h) return;
  const dlt = h.point.clone().sub(controls.target);
  controls.target.copy(h.point);
  camera.position.add(dlt);
  focusMoved();
});
let moveTimer = null;
function focusMoved() {
  clearTimeout(moveTimer);
  moveTimer = setTimeout(() => { updateVisibility(); updateSpot(); }, 60);
}
controls.addEventListener('change', () => {
  if (!controls.target.equals(focusDot.position)) focusMoved();
});

$('copy').onclick = () => {
  const out = {
    gen: meta.gen, radii: st.radii, global: Object.fromEntries(LEVELS.map((l) => [l, st.glob[l] === OFF ? 'off' : meta.ratios[st.glob[l]]])),
    cells: [...st.ovr].map(([k, o]) => {
      const [b, c] = k.split(':').map(Number);
      return { block: meta.blocks[b].name, cell: meta.blocks[b].cells[c].key, pick: Object.fromEntries(Object.entries(o).map(([l, r]) => [l, r === OFF ? 'off' : meta.ratios[r]])) };
    }),
    budget: last && { room_kb: last.room_kb, total: last.total, hard: { p90: last.hard.p90, max: last.hard.max, gate_pct: last.hard.world_over_or_allowance_pct } },
  };
  navigator.clipboard.writeText(JSON.stringify(out, null, 1)).then(() => { $('copy').textContent = 'Copied'; setTimeout(() => { $('copy').textContent = 'Copy choices JSON'; }, 1200); });
};
$('reset').onclick = () => {
  LEVELS.forEach((l) => { st.glob[l] = 0; });
  st.ovr.clear();
  Object.assign(st.radii, meta.radii);
  renderRadii();
  changed();
};

function gotoBlock(b) {
  const s = meta.blocks[b].spots;
  const n = s.length / 3, c = [0, 0, 0];
  for (let i = 0; i < s.length; i += 3) { c[0] += s[i]; c[1] += s[i + 1]; c[2] += s[i + 2]; }
  controls.target.set(c[0] / n, c[1] / n, c[2] / n);
  camera.position.set(c[0] / n + 50, c[1] / n + 140, c[2] / n + 50);
  controls.update();
  focusMoved();
}

// ---------- start ----------
async function start() {
  resize();
  meta = await fetch('/api/meta').then((r) => r.json());
  st.radii = { ...meta.radii };
  $('gen').textContent = `${meta.gen}: ${meta.blocks.map((b) => b.name).join(', ')}`;
  $('ref').innerHTML = meta.reference.map((r) => `<tr><td>${r.name}<div class="sub">${r.src}</div></td><td class="n">${r.value}</td></tr>`).join('');
  meta.blocks.forEach((blk, b) => {
    objs[b] = {};
    const g = new THREE.Group();
    world.add(g);
    LEVELS.forEach((lev) => {
      objs[b][lev] = new Map();
      blk.cells.forEach((cell, c) => {
        if (!cell.tris[LEVELS.indexOf(lev)]) return;
        const m = new THREE.Mesh(new THREE.BufferGeometry(), mats.texture);
        m.userData = { block: b, cell: c, level: lev };
        objs[b][lev].set(c, m);
        g.add(m);
      });
    });
    fetchMesh(b, 'sky', 0).then((mm) => {
      const geo = mm.get(-1);
      if (!geo) return;
      skyObjs[b] = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.DoubleSide }));
      skyObjs[b].visible = st.sky;
      scene.add(skyObjs[b]);
    });
    const pg = new THREE.BufferGeometry();
    pg.setAttribute('position', new THREE.BufferAttribute(new Float32Array(blk.spots), 3));
    pg.setAttribute('color', new THREE.BufferAttribute(new Float32Array(blk.spots.length).fill(0.6), 3));
    spotPts[b] = new THREE.Points(pg, new THREE.PointsMaterial({ size: 3, sizeAttenuation: false, vertexColors: true }));
    scene.add(spotPts[b]);
    const o = document.createElement('option');
    o.value = b; o.textContent = blk.name;
    $('goto').appendChild(o);
  });
  $('goto').onchange = (e) => gotoBlock(+e.target.value);
  setupRadii();
  renderRadii();
  renderView();
  renderLevels();
  gotoBlock(0);
  await applyGeometry();
  runBudget();
}
(function loop() { requestAnimationFrame(loop); controls.update(); renderer.render(scene, camera); })();
start().catch((e) => { $('hud').textContent = 'Error: ' + e; console.error(e); });
