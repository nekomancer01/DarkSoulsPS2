"""Normal-map relief for the PS2 map (CLUT dot3, TECH_DEBT map-23): bump.bin, a pack entry next to map.bin.

DSR lights map pieces per pixel with a tangent-space normal map (g_Bumpmap, BC5). FRPG_Phn_*Bmp*.fpo (RenderDoc,
Firelink) decodes x = 2r - 1, y = 2g - 1, z = sqrt(1 - x^2 - y^2) at strength p = gFC_LightProbeParam.z (1.0 on all
157 draws at spot A): n = normalize(N (p (z - 1) + 1) + B p x + T p y), T = the FLVER tangent (vertex semantic 6),
B = w (N x T) with w = the tangent's .w (FRPG_Phn_PINT_*.vpo). The GS has no per-pixel lighting, so the export does:
- textures: every bump texture in use -> an 8-bit index map into one area-wide palette of normals (index 0 = flat,
  1..255 = k-means over the texels in use). Mip 1 averages x, y (as the BC5 mips the GPU filters) and maps to the
  same palette, so distant relief flattens like DSR's.
- clusters: bumped triangles (MTD letter block with B: M[DB], M[DB][M]; lit, opaque, not cut-out / foliage) get a
  lighting response: DSR's light (the part's LightBank row: directional + hemisphere + GI_EnvDif probe, as
  dsr_light.light_fn) x the baked sun shadow, through the area's tone map, at RING tangent-space normals. An optional
  coarse k-means (BUMP_SPLIT groups, default off) keeps very different triangles out of one chunk (build_chunks); the
  chunks, by their mean response, go into BUMP_K clusters (one CLUT per chunk, so the chunk count does not grow).
- CLUTs: per cluster a CT32 CLUT, alpha = 0x80 x DSR's output for the palette normal / its output for the flat normal
  (what the vertex colour already holds), averaged over a sample of the cluster's triangles; rgb unused. Level 1 is the
  half-strength version (ratio^0.5) for the far band.
ps2/proto/src/bump.c draws the bumped chunks a second time with their index map and cluster CLUT: ALPHA Cd x As,
depth test GEQUAL on the depth the opaque pass wrote, no Z write, no fog.

bump.bin (little-endian, 16-byte aligned sections, pixels 128-byte aligned):
  header  u32 'DSB1', chunks (= map.bin's chunk count, 0 = no relief), textures, clusters, levels; u32 offsets:
          chunk table, texture table, CLUTs; u32 cap (relief batches per frame, 0 = none); pad to 48 bytes
  chunk table  per map.bin chunk: u16 texture (0xFFFF = not bumped), u8 cluster, u8 relief strength = std of the
               level 0 CLUT alpha over the texture's mip 0 texels (0x80 = x1; bump.c skips chunks below one 5-bit
               frame buffer step)
  textures     tools/texmap.py entries (32 B: psm PSMT8, clut psm CT32, clut offset 0 = none; pixel offsets from the
               start of bump.bin)
  CLUTs        [cluster][level] 16x16 CT32, CSM1-ordered (1 KB each)
  pixels       per texture mip 0, mip 1 (8-bit, linear rows)
"""
import math, os, re, struct

import numpy as np

import dsr_light
import souls
import texmap

BUMP = os.environ.get("BUMP", "1") == "1"
BUMP_MAX = int(os.environ.get("BUMP_MAX", 128))  # px, largest normal-map side (VRAM pool / RAM)
BUMP_K = int(os.environ.get("BUMP_K", 48))       # lighting clusters = CLUTs, one per chunk
# coarse per-triangle response groups that build_chunks keeps apart (a wall's sunlit and shadowed sides). Measured at
# Firelink (CLUT error = mean |triangle - CLUT| at the palette normals, map pass at spot A, PCSX2): per-triangle
# clusters (K 48) 11.2k chunks of 9 tris; 6 groups 5217 chunks, error 25 / 128, +1.85 ms; 1 (none) 5032 chunks
# (no relief: 5023), error 29 / 128, +0.79 ms. Default 1: the split cost more than the relief.
BUMP_SPLIT = int(os.environ.get("BUMP_SPLIT", 1))
# relief batches per frame, nearest first (0 = no cap): a per-area cost cap, set from the Makefile's MAPENV_<area>
# (Undead Burg: 32 keeps the southwest spot at 55 fps; uncapped 49, relief off 59)
BUMP_CAP = int(os.environ.get("BUMP_CAP", 0))
LEVELS = 2
STRENGTH = 1.0  # gFC_LightProbeParam.z, MEASURED (RenderDoc, Firelink spot A, every bump draw)
# tangent-space normals the clustering compares responses at: flat, then rings at 25 and 50 degrees
RING = [(0.0, 0.0)] + [(math.sin(t) * math.cos(a), math.sin(t) * math.sin(a))
                       for t in (math.radians(25), math.radians(50)) for a in np.arange(8) * math.pi / 4]
# output-space floor of the ratio: one step of the 5-bit frame buffer, so near-black texels (deep shadow, where the flat
# output is ~0) do not turn tiny absolute differences into huge ratios
EPS = 1 / 32
# split groups smaller than this (triangles in one texture + cell) join the largest bumped group of that texture + cell
# instead of becoming a partial chunk of their own
BUMP_MIN_GROUP = int(os.environ.get("BUMP_MIN_GROUP", 18))
CLUT_SAMPLE = 64  # triangles per cluster the CLUT is averaged over

MESH = []         # per registered mesh: dict(tex, light, alb, N, T, W, probe_of)
SRC = {}          # gather(): dds, probes, near (scattering d = 0 factor), smap (sun shadow map)
CHUNK_BUMP = []   # per build_chunks output chunk: (split key, [triangle numbers]) or None (filled by build_chunks)
_STATE = {}       # assign() -> write(): textures, palette, CLUTs, densities


def bumped(mtd):
    """MTDs whose shader reads g_Bumpmap: a B in the letter block ([DB], [DSB], [DB][M]); cut-outs (_Edge), blends
    (_Alp / _Add) and foliage / trees are left out (a second pass over cut-out texels would darken what is behind)."""
    low = mtd.lower()
    return bool(re.search(r"\[[a-z]*b[a-z]*\]", low)) and not any(k in low for k in ("_edge", "_alp", "_add",
                                                                                    "foliage", "ivy", "tree"))


def register(m, V, fvw, wn, xn, light_row, alb, probe_of):
    """gather() hook for one lit map mesh: keeps what the relief needs per wedge (normal, tangent, handedness) and
    returns an index for the mesh dict's "bump" key, or None when the mesh has no relief."""
    if not BUMP or not m.get("bump") or not bumped(m["mtd"]) or len(V.get("tangent") or ()) != len(V["pos"]):
        return None
    nw = len(wn)
    T, W = np.zeros((nw, 3)), np.zeros(nw)
    for vi, w in fvw.items():
        t = V["tangent"][vi]
        T[w] += xn(t[:3])
        W[w] += t[3]
    N = np.array(wn, np.float64)
    MESH.append(dict(tex=m["bump"], light=light_row, alb=alb, N=N, T=T, W=np.where(W < 0, -1.0, 1.0),
                     probe_of=probe_of))
    return len(MESH) - 1


# ---- DSR's light, vectorised (mirrors dsr_light.light_fn / probe_sample; checked against them in assign) ----
def _light_terms(row):
    dirs = [(np.array(dsr_light.light_dir(row[f"degRotX_{i}"], row[f"degRotY_{i}"])),
             np.array([dsr_light.lin(row[f"col{c}_{i}"], row[f"colA_{i}"]) for c in "RGB"]))
            for i in "012" if row[f"colA_{i}"] > 0]
    up = np.array([dsr_light.lin(row[f"col{c}_u"], row["colA_u"]) for c in "RGB"])
    down = np.array([dsr_light.lin(row[f"col{c}_d"], row["colA_d"]) for c in "RGB"])
    env = np.array([dsr_light.lin(row[f"envDif_col{c}"], row["envDif_colA"]) for c in "RGB"])
    return dirs, up, down, env


def _probe(faces, n):
    R = dsr_light.PROBE_RES
    ax = np.argmax(np.abs(n), 1)
    rows = np.arange(len(n))
    m = np.abs(n[rows, ax])
    m = np.where(m > 0, m, 1.0)
    f = np.where(n[rows, ax] > 0, 0, 1) + 2 * ax
    out = np.zeros((len(n), 3))
    for fi, (_, _, (sa, ss), (ta, ts)) in enumerate(dsr_light._FACE):
        k = f == fi
        if not k.any():
            continue
        u = np.minimum(R - 1, ((ss * n[k, sa] / m[k] + 1) / 2 * R).astype(int))
        v = np.minimum(R - 1, ((ts * n[k, ta] / m[k] + 1) / 2 * R).astype(int))
        out[k] = faces[fi, v, u]
    return out


def _irradiance(terms, faces, n):
    """n (M, 3) unit -> (ambient, directional) linear HDR rgb (M, 3) each"""
    dirs, up, down, env = terms
    h = n[:, 1:2] * 0.5 + 0.5
    e = _probe(faces, n) if faces is not None else 0.0
    amb = down + (up - down) * h + env * e
    sun = np.zeros_like(amb)
    for d, col in dirs:
        sun += np.maximum(0.0, n @ d)[:, None] * col
    return amb, sun


def _frames(N, T, W):
    """per-row DSR tangent frame: unit N, T, B = w (N x T)"""
    N = N / (np.linalg.norm(N, axis=1, keepdims=True) + 1e-12)
    T = T / (np.linalg.norm(T, axis=1, keepdims=True) + 1e-12)
    B = W[:, None] * np.cross(N, T)
    return N, T, B


def _perturb(N, T, B, xy):
    """tangent-space normals xy (J, 2) on every frame row -> world normals (rows, J, 3), DSR's formula"""
    x, y = STRENGTH * xy[:, 0], STRENGTH * xy[:, 1]
    z = STRENGTH * (np.sqrt(np.maximum(0.0, 1 - xy[:, 0] ** 2 - xy[:, 1] ** 2)) - 1) + 1
    n = N[:, None, :] * z[None, :, None] + B[:, None, :] * x[None, :, None] + T[:, None, :] * y[None, :, None]
    return n / (np.linalg.norm(n, axis=2, keepdims=True) + 1e-12)


class _Response:
    """DSR output (tone mapped, gamma) / output for the flat normal, per triangle at given tangent-space normals."""

    def __init__(self, tone, near, shadow_amb):
        self.near, self.sa = np.array(near), shadow_amb
        y = np.logspace(-6, 3, 1024)
        self.ly = np.log(y)
        self.out = np.array([sum(k * v for k, v in zip(dsr_light.LUMA, tone([v, v, v]))) for v in y])
        self.terms, self.faces = {}, {}

    def tone(self, Y):
        return np.interp(np.log(np.maximum(Y, 1e-6)), self.ly, self.out)

    def __call__(self, tri, xy):
        """tri: dict of arrays (N, T, B, k, alb (rgb linear), light id, probe id, rows / probes lookups) -> (n, J)"""
        nt = len(tri["N"])
        n = _perturb(tri["N"], tri["T"], tri["B"], np.vstack([[0.0, 0.0], xy]))  # column 0: flat
        J = n.shape[1]
        Y = np.zeros((nt, J))
        for key in set(zip(tri["lid"], tri["pid"])):
            sel = np.nonzero((tri["lid"] == key[0]) & (tri["pid"] == key[1]))[0]
            terms = self.terms.setdefault(key[0], _light_terms(tri["rows"][key[0]]))
            faces = tri["probes"].get(key[1])
            amb, sun = _irradiance(terms, faces, n[sel].reshape(-1, 3))
            k = np.repeat(tri["k"][sel], J)[:, None]
            e = amb * (1 - self.sa * (1 - k)) + sun * k
            alb = np.repeat(tri["alb"][sel], J, axis=0)
            Y[sel] = ((e * alb * self.near) @ np.array(dsr_light.LUMA)).reshape(len(sel), J)
        o = self.tone(Y) + EPS
        return np.clip(o[:, 1:] / o[:, :1], 0, 255 / 128)


# ---- normal maps ----
def _bc4(blk):
    """(n, 8) BC4 blocks -> (n, 16) values 0..1"""
    a0, a1 = blk[:, 0].astype(np.float32), blk[:, 1].astype(np.float32)
    bits = np.zeros(len(blk), np.uint64)
    for i in range(6):
        bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = np.stack([(bits >> np.uint64(3 * j)) & np.uint64(7) for j in range(16)], 1).astype(int)
    pal = np.zeros((len(blk), 8), np.float32)
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        pal[:, 1 + i] = np.where(big, ((7 - i) * a0 + i * a1) / 7, ((5 - i) * a0 + i * a1) / 5 if i < 5 else 0)
    pal[~big, 6], pal[~big, 7] = 0, 255
    return np.take_along_axis(pal, idx, 1) / 255.0


def decode_xy(d, max_dim):
    """normal-map DDS (BC5, or DXT1 / BC7 via souls) -> (h, w, 2) tangent x, y in -1..1, largest mip <= max_dim"""
    h, w, mips = struct.unpack_from("<iii", d, 12)
    mips = max(1, mips)
    four = d[84:88]
    if four == b"DX10" and struct.unpack_from("<I", d, 128)[0] in (83, 84):  # BC5_UNORM / SNORM (DSR: 83)
        off = 148
        while mips > 1 and max(w, h) > max_dim:
            off += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
            w, h, mips = max(1, w // 2), max(1, h // 2), mips - 1
        bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
        blk = np.frombuffer(d, np.uint8, bw * bh * 16, off).reshape(-1, 16)
        ch = [_bc4(blk[:, :8]), _bc4(blk[:, 8:])]
        img = np.stack([c.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4) for c in ch], 2)
        img = img[:h, :w]
    else:
        dec = souls.dds_decode_rgba(d, max_dim)
        if not dec:
            return None
        w, h = dec[0], dec[1]
        img = np.frombuffer(dec[2], np.uint8).reshape(h, w, 4)[:, :, :2] / 255.0
    return img * 2 - 1


def _pow2(img, cap):
    """resample to power-of-two sides <= cap (box average down, nearest up)"""
    h, w = img.shape[:2]
    tw, th = 1, 1
    while tw * 2 <= min(w, cap):
        tw *= 2
    while th * 2 <= min(h, cap):
        th *= 2
    ys = (np.arange(th)[:, None] * h // th + np.arange(max(1, h // th))[None, :]).clip(0, h - 1)
    xs = (np.arange(tw)[:, None] * w // tw + np.arange(max(1, w // tw))[None, :]).clip(0, w - 1)
    return img[ys][:, :, xs].mean(axis=(1, 3))


def _kmeans(x, k, wts=None, iters=20, seed=1):
    """weighted Lloyd k-means, k-means++ start; returns (centres, labels)"""
    rng = np.random.default_rng(seed)
    wts = np.ones(len(x)) if wts is None else wts
    k = min(k, len(x))
    c = [x[rng.choice(len(x), p=wts / wts.sum())]]
    d2 = ((x - c[0]) ** 2).sum(1)
    for _ in range(1, k):
        p = d2 * wts
        c.append(x[rng.choice(len(x), p=p / p.sum())] if p.sum() > 0 else x[rng.integers(len(x))])
        d2 = np.minimum(d2, ((x - c[-1]) ** 2).sum(1))
    c = np.array(c)
    for _ in range(iters):
        lab = _nearest(x, c)
        # members of each cluster in index order (a stable sort by label: the same rows as the mask lab == j, without
        # k passes over x)
        order = np.argsort(lab, kind="stable")
        bounds = np.searchsorted(lab[order], np.arange(k + 1)).tolist()
        for j in range(k):
            if bounds[j] < bounds[j + 1]:
                s = order[bounds[j]:bounds[j + 1]]
                c[j] = (x[s] * wts[s, None]).sum(0) / wts[s].sum()
    return c, _nearest(x, c)


def _nearest(x, c, step=65536):
    out = np.empty(len(x), int)
    cc = (c ** 2).sum(1)
    for i in range(0, len(x), step):
        d = (2 * x[i:i + step]) @ c.T
        np.subtract(cc, d, out=d)  # cc - 2 x.c in place: the same values without a second (step, k) array
        out[i:i + step] = d.argmin(1)
    return out


# ---- export ----
def merge_small(groups):
    """build_chunks hook: groups {(texture, flags, cell, split key or None): [(fv, k)]} -> the same with bumped groups
    under BUMP_MIN_GROUP triangles merged into the largest bumped group of the same (texture, flags, cell)."""
    best = {}
    for key, ks in groups.items():
        if key[3] is not None and len(ks) > len(groups.get(best.get(key[:3]), ())):
            best[key[:3]] = key
    out = {}
    for key, ks in groups.items():
        dst = best[key[:3]] if key[3] is not None and len(ks) < BUMP_MIN_GROUP else key
        out.setdefault(dst, []).extend(ks)
    return out



def assign(meshes, dds, smap, tone, near, shadow_amb, probes):
    """After simplification (mesh "tris" = corners (position, wedge)): cluster the bumped triangles and build the
    textures and CLUTs. Returns {triangle number in main()'s order: (texture name, cluster)} for build_chunks."""
    _STATE.clear()
    tris = []  # (global triangle number, mesh entry, corners)
    k = 0
    for m in meshes:
        tl = m["tris"]
        if not tl:
            continue
        e = MESH[m["bump"]] if m.get("bump") is not None else None
        if e is not None and e["tex"] in dds:
            tris += [(k + i, e, t) for i, t in enumerate(tl)]
        k += len(tl)
    if not tris:
        print("bump: no bumped triangles")
        return {}
    # per triangle: DSR frame (mean of its corners), sun visibility (baked shadow at the corners), light row, probe
    nt = len(tris)
    N, T, W = np.zeros((nt, 3)), np.zeros((nt, 3)), np.zeros(nt)
    ks, area, lid, pid, alb = np.ones(nt), np.zeros(nt), np.zeros(nt, int), np.zeros(nt, int), np.zeros((nt, 3))
    rows, rid, faces = {}, {}, {}
    # vectorised per mesh entry (was one numpy call chain per triangle: 68 s of Firelink's export); every element is
    # computed with the same operations in the same order as before (row sums ((a + b) + c), np.cross, the vector norm
    # per row), so the clusters and textures are unchanged
    WS = np.array([[c[1] for c in t] for _, _, t in tris], np.int64).reshape(-1, 3)
    PS = np.array([[c[0] for c in t] for _, _, t in tris], np.float64).reshape(-1, 3, 3)
    ent, eix = {}, np.empty(nt, np.int64)
    for i, (_, e, _) in enumerate(tris):
        eix[i] = ent.setdefault(id(e), (len(ent), e))[0]
    for k, e in ent.values():
        sel = np.nonzero(eix == k)[0]
        w = WS[sel]
        N[sel] = e["N"][w[:, 0]] + e["N"][w[:, 1]] + e["N"][w[:, 2]]
        T[sel] = e["T"][w[:, 0]] + e["T"][w[:, 1]] + e["T"][w[:, 2]]
        W[sel] = e["W"][w[:, 0]] + e["W"][w[:, 1]] + e["W"][w[:, 2]]
        lid[sel] = rid.setdefault(id(e["light"]), len(rid))
        rows[lid[sel[0]]] = e["light"]
        alb[sel] = [dsr_light.srgb_lin(a) for a in e["alb"]]
    C = np.cross(PS[:, 1] - PS[:, 0], PS[:, 2] - PS[:, 0])
    norm = np.linalg.norm  # per row: np.linalg.norm(x, axis=1) sums differently (BLAS dot vs reduce)
    area[:] = [0.5 * norm(c) for c in C]
    Nn = N / (np.array([norm(v) for v in N]).reshape(-1, 1) + 1e-12)
    if smap is not None:
        k3 = smap.lit_many(PS.reshape(-1, 3), np.repeat(Nn, 3, axis=0)).reshape(-1, 3)
        ks[:] = (k3[:, 0] + k3[:, 1] + k3[:, 2]) / 3
    cen = ((PS[:, 0] + PS[:, 1] + PS[:, 2]) / 3).tolist()
    for i, (_, e, _) in enumerate(tris):
        p = e["probe_of"](tuple(cen[i]))
        pid[i] = p if p is not None else -1
        if pid[i] not in faces and pid[i] in probes:
            faces[pid[i]] = np.array(probes[pid[i]], np.float64)
    N, T, B = _frames(N, T, np.where(W < 0, -1.0, 1.0))
    tri = dict(N=N, T=T, B=B, k=ks, alb=alb, lid=lid, pid=pid, rows=rows, probes=faces)
    # check the vectorised light against dsr_light (same row / probe, random normals)
    rn = np.random.default_rng(0).normal(size=(16, 3))
    rn /= np.linalg.norm(rn, axis=1, keepdims=True)
    f = dsr_light.light_fn(rows[0])
    pf = faces.get(pid[0])
    a, s = _irradiance(_light_terms(rows[0]), pf, rn)
    ref = [f(tuple(v), probes.get(pid[0]), split=True) for v in rn]
    err = max(np.abs(a - np.array([r[0] for r in ref])).max(), np.abs(s - np.array([r[1] for r in ref])).max())
    assert err < 1e-6, f"vectorised light differs from dsr_light by {err}"
    resp = _Response(tone, near, shadow_amb)
    R = resp(tri, np.array(RING[1:]))
    _, lab = _kmeans(R, BUMP_SPLIT, np.sqrt(area + 1e-6))
    # textures in use, the palette, index maps
    names = sorted({e["tex"] for _, e, _ in tris})
    imgs = {}
    for nme in names:
        xy = decode_xy(dds[nme], max(BUMP_MAX, 4))
        if xy is not None:
            imgs[nme] = _pow2(xy, BUMP_MAX)
    allxy = np.concatenate([v.reshape(-1, 2) for v in imgs.values()])
    sub = allxy[np.random.default_rng(2).choice(len(allxy), min(len(allxy), 150000), replace=False)]
    pal, _ = _kmeans(sub, 255, iters=15)
    pal = np.vstack([[0.0, 0.0], pal])
    texs, err_px = [], []
    for nme in names:
        if nme not in imgs:
            continue
        x0 = imgs[nme]
        h, w = x0.shape[:2]
        x1 = x0.reshape(h // 2, 2, w // 2, 2, 2).mean(axis=(1, 3)) if h > 1 and w > 1 else x0
        mips = [_nearest(x.reshape(-1, 2), pal).astype(np.uint8) for x in (x0, x1)]
        err_px.append(np.linalg.norm(pal[mips[0]] - x0.reshape(-1, 2), axis=1).mean())
        texs.append(dict(name=nme, w=w, h=h, mips=[mm.tobytes() for mm in mips]))
    tex_of = {t["name"]: i for i, t in enumerate(texs)}
    # density (texels per metre, area-weighted) per texture for texcache's mip choice
    uva, wa = np.zeros(len(texs)), np.zeros(len(texs))
    out, local = {}, {}
    for i, (g, e, t) in enumerate(tris):
        if e["tex"] not in tex_of:
            continue
        ti = tex_of[e["tex"]]
        out[g] = (ti, int(lab[i]))
        local[g] = i
        wa[ti] += area[i]
    for m in meshes:
        e = MESH[m["bump"]] if m.get("bump") is not None and m["tris"] else None
        if e is None or e["tex"] not in tex_of:
            continue
        ti = tex_of[e["tex"]]
        for t in m["tris"]:
            uv = np.array([m["wuv"][c[1]] for c in t]) * [texs[ti]["w"], texs[ti]["h"]]
            d1, d2 = uv[1] - uv[0], uv[2] - uv[0]
            uva[ti] += 0.5 * abs(d1[0] * d2[1] - d1[1] * d2[0])
    _STATE["density"] = [float(math.sqrt(u / a)) if a > 0 else 0.0 for u, a in zip(uva, wa)]
    _STATE.update(texs=texs, tri=tri, R=R, area=area, local=local, resp=resp, pal=pal)
    print(f"bump: {nt} triangles ({100 * nt / max(1, k):.0f}%), {len(texs)} textures (<= {BUMP_MAX} px), "
          f"palette error {np.mean(err_px):.3f} (xy)")
    return out


def _finish():
    """chunks (CHUNK_BUMP) -> BUMP_K clusters by area-weighted mean response; CLUT per cluster = the mean response at
    every palette normal over a sample of its triangles. Returns (per chunk (texture, cluster) or None, CLUTs)."""
    R, area, local, tri, resp = (_STATE[k] for k in ("R", "area", "local", "tri", "resp"))
    ids = [i for i, b in enumerate(CHUNK_BUMP) if b]
    mem = [np.array([local[g] for g in CHUNK_BUMP[i][1] if g in local]) for i in ids]
    F = np.array([(R[m] * area[m, None]).sum(0) / max(1e-9, area[m].sum()) for m in mem])
    cen, lab = _kmeans(F, BUMP_K, np.array([area[m].sum() for m in mem]) + 1e-6)
    rng = np.random.default_rng(3)
    cluts, cerr, terr = [], [], []
    for c in range(len(cen)):
        tm = np.concatenate([mem[j] for j in np.nonzero(lab == c)[0]] or [np.zeros(0, int)])
        if not len(tm):
            cluts.append(np.ones(256))
            continue
        smp = tm if len(tm) <= CLUT_SAMPLE else rng.choice(tm, CLUT_SAMPLE, replace=False)
        r = resp({key: (v[smp] if isinstance(v, np.ndarray) else v) for key, v in tri.items()}, _STATE["pal"])
        cluts.append(r.mean(0))
        cerr.append(np.abs(r - r.mean(0)).mean() * 128)
        terr.append(np.abs(R[tm] - cen[c]).mean() * 128)
    per = [None] * len(CHUNK_BUMP)
    for j, i in enumerate(ids):
        per[i] = (CHUNK_BUMP[i][0][0], int(lab[j]))
    cl = np.clip(np.array(cluts), 0, 255 / 128)
    print(f"bump: {len(ids)} chunks in {len(cen)} clusters, |triangle - CLUT| {np.mean(cerr):.1f} / 128 over the "
          f"palette, {np.mean(terr):.1f} / 128 at the ring normals; ratio {cl.min():.2f}..{cl.max():.2f}")
    return per, cl


def _blocks8(w, h):
    """VRAM blocks of a PSMT8 level: texmap.blocks for <= 128 px wide (buffer width 128); wider ones use buffer width w
    (vu1.c TBW, the upload's), whole 128 x 64 pages of 32 blocks, a power of two"""
    if w <= 128:
        return texmap.blocks(texmap.PSMT8, w, h)
    n, top = 1, -(-w // 128) * -(-h // 64) * 32
    while n < top:
        n *= 2
    return n


def write(path, nchunks):
    """bump.bin for the chunks build_chunks made (CHUNK_BUMP); an empty one (chunks 0) when there is no relief."""
    texs = _STATE.get("texs")
    HDR = 48
    if not texs or len(CHUNK_BUMP) != nchunks or not any(CHUNK_BUMP):
        with open(path, "wb") as f:
            f.write(struct.pack("<4s7I", b"DSB1", 0, 0, 0, 0, 0, 0, 0) + b"\0" * (HDR - 32))
        return
    per, cl = _finish()
    blob = bytearray()

    def align(n):
        blob.extend(b"\0" * ((-(len(blob) + HDR)) % n))
    ctab = HDR + len(blob)
    pix0 = [np.frombuffer(t["mips"][0], np.uint8) for t in texs]
    spread = {}
    for b in per:
        if b and b not in spread:
            spread[b] = min(255, int(round(float((128 * cl[b[1]][pix0[b[0]]]).std()))))
        blob += struct.pack("<HBB", *(b if b else (0xFFFF, 0)), spread[b] if b else 0)
    align(16)
    ttab = HDR + len(blob)
    tpos = len(blob)
    blob += b"\0" * (texmap.ENTRY * len(texs))
    align(16)
    coff = HDR + len(blob)
    sw = [texmap._swz(i) for i in range(256)]
    for c in range(len(cl)):
        for lv in range(LEVELS):
            a = np.clip(np.round(128 * cl[c] ** (1.0 / (1 << lv))), 0, 255).astype(int)
            e = bytearray(1024)
            for i in range(256):
                e[4 * sw[i]:4 * sw[i] + 4] = bytes((0x80, 0x80, 0x80, a[i]))
            blob += e
    for k, t in enumerate(texs):
        offs = []
        for d in t["mips"]:
            align(128)
            offs.append(HDR + len(blob))
            blob += d
        b0, b1 = _blocks8(t["w"], t["h"]), _blocks8(t["w"] // 2, t["h"] // 2)
        struct.pack_into("<2H4BfIIIHHI", blob, tpos + texmap.ENTRY * k, t["w"], t["h"], texmap.PSMT8, texmap.CT32,
                         2, 0, _STATE["density"][k], 0, offs[0], offs[1], b0, b1, 0)
    hdr = struct.pack("<4s8I", b"DSB1", nchunks, len(texs), len(cl), LEVELS, ctab, ttab, coff, BUMP_CAP)
    with open(path, "wb") as f:
        f.write(hdr + b"\0" * (HDR - len(hdr)) + blob)
    nb = sum(1 for b in CHUNK_BUMP if b)
    sp = np.array([spread[b] for b in per if b])
    print(f"bump.bin: relief strength (std / 128) median {np.median(sp):.0f}, < 4: {100 * (sp < 4).mean():.0f}% of chunks")
    print(f"bump.bin: {nb} of {nchunks} chunks bumped, {len(texs)} textures, {len(cl)} x {LEVELS} CLUTs, "
          f"{HDR + len(blob)} bytes")
