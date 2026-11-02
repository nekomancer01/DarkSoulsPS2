"""Map textures for the PS2 texture cache (ps2/proto/src/texcache.c): 4/8-bit indexed, CT16 or CT32 CLUT, mip 0 + 1.

encode(): RGBA from the DDS -> one texture record. Width is scaled to 128 (PSMT8/4 buffer width) and the height capped
at 128 like before. Opaque / cut-out textures: 16 colours (PSMT4) when that keeps PSNR >= T4_PSNR dB on the opaque
texels, else 255 colours (PSMT8); the last index is transparent (alpha < 50 %). Their CLUT is CT16 (RGBA 5551; the
GS expands the alpha bit with TEXA TA0 = 0, TA1 = 0x80). Blended effects keep 256 colours with alpha in a CT32 CLUT.
Mip 1 is the half-size image (box filter on premultiplied colour) mapped to the same palette; mips 2, 3 (MIP_CHAIN)
the same from mip 1's filtered image, for the GS mip chain.

Section written by write() (offsets from the start of map.bin):
  entry {u16 w, h (mip 0); u8 psm (0x13 PSMT8 / 0x14 PSMT4), clut psm (0 CT32 / 2 CT16), levels, flags (bit 0: near
         level); f32 texels per metre (mip 0, area-weighted over its triangles); u32 clut, pix0, pix1 offsets;
         u16 VRAM blocks (256 B) mip 0, mip 1 (power of two covering every block the level touches, buffer width 128);
         u32 near level pixels (2w x 2h, same palette, for near chunks; texcache.c level 2) or 0}  (32 B)
  then per texture: CLUT (PSMT8: 16x16 CSM1-ordered, PSMT4: 8x2), mip 0, mip 1 (, mip 2, mip 3) pixels, each 16-byte
  aligned (levels > 2: mip 2 and 3 follow mip 1, texcache.c finds them by size).
"""
import struct
import numpy as np

PSMT8, PSMT4, CT32, CT16 = 0x13, 0x14, 0x00, 0x02
# Colour fidelity first (map-20: the port already looks duller than DSR). Metrics against the source, opaque texels,
# no fog / light (_metrics). PSMT4 only when the 16-colour version keeps the colour about as well as the 8-bit one:
T4_PSNR = 31.0      # dB
T4_CHROMA = 0.95    # chroma kept, and at most T4_CHROMA_REL below the 8-bit version's
T4_CHROMA_REL = 0.03
T4_HUE = 3.0        # degrees more hue shift (coloured texels) than the 8-bit version, at most
T4_TINT = 2.0       # degrees more shift of the mean tint than the 8-bit version, at most
T4_CONTRAST = 0.95  # luma standard deviation kept
# CT16 CLUT (5 bits per channel) only when it costs no visible colour against CT32 on the chosen depth:
CT16_HUE, CT16_TINT, CT16_CHROMA = 1.5, 1.0, 0.02
# Per-texture override (DDS name -> 4 or 8), applied after the rule; also env TEX_FORCE4 / TEX_FORCE8 (comma lists).
PSM_OVERRIDE = {}
# Palette builder for opaque / cut-out textures (env TEX_QUANT): "liq" = libimagequant (tools/liq, speed 1) on the
# opaque texels, texels mapped to the nearest entry (no dithering); "kmeans" = Pillow median cut, + Lloyd refinement for
# 16 colours (the quantiser before 2026-10-31). Measured on Firelink's 80 textures with _metrics (tools/liq/bench_quant.py,
# notes/threads/tools-assets.md): liq 8-bit 54.9 dB / hue 1.44 deg vs 52.4 / 1.92, 4-bit +0.66 dB median; the rule
# below then picks PSMT4 for 23 instead of 30 textures (+5 % VRAM). Falls back to kmeans if the library is missing.
QUANT = __import__("os").environ.get("TEX_QUANT", "liq")
REPORT = []  # (name, source, 8-bit, 4-bit images, metrics, chosen) for sheet(); filled by encode
ENTRY = 32
# GS mip chain below mip 1 (render-12: far walls shimmer without it): mip 2, mip 3 stored after mip 1, levels = 2 + this.
# texcache.c draws them with MXL / MIPTBP1 under a chunk's mip 0 / 1. MIP_CHAIN=0: the old two levels (A/B)
MIP_CHAIN = int(__import__("os").environ.get("MIP_CHAIN", 2))

# GS block numbers inside a page: PSMT8 page 128x64 (blocks 16x16, 8 x 4), PSMT4 page 128x128 (blocks 32x16, 4 x 8)
_BLK8 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29],
         [10, 11, 14, 15, 26, 27, 30, 31]]
_BLK4 = [[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15],
         [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]]


def blocks(psm, w, h):
    """VRAM footprint in 256-byte blocks (rounded up to a power of two) of a w x h texture at buffer width 128."""
    bw, bh, pw, ph, tab = (16, 16, 128, 64, _BLK8) if psm == PSMT8 else (32, 16, 128, 128, _BLK4)
    top = 0
    for y in range(0, h, bh):
        for x in range(0, w, bw):
            b = (y // ph) * 32 + tab[(y % ph) // bh][(x % pw) // bw]
            top = max(top, b + 1)
    n = 1
    while n < top:
        n *= 2
    return n


def _swz(i):
    return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


def _q5(c):
    """8-bit channel -> CT16 5-bit value; the GS expands it back as v << 3 (TEXA does not touch colour)."""
    return np.minimum(31, (np.asarray(c, np.int32) + 4) >> 3)


def _ct16(r, g, b, a):
    return int(_q5(r)) | int(_q5(g)) << 5 | int(_q5(b)) << 10 | (0x8000 if a else 0)


def _metrics(src, out):
    """Colour fidelity of out against src ((N,3) uint8, opaque texels): PSNR dB; chroma ratio (mean length of the
    opponent colour vector a = R - G, b = (R + G) / 2 - B, out / src); hue shift in degrees (angle between the src and
    out opponent vectors, weighted by the source chroma squared, over texels with chroma > 16: grey stone has no
    stable hue); tint shift in degrees (angle between the mean opponent vectors); luma contrast ratio."""
    a, b = src.astype(np.float32), out.astype(np.float32)
    e = ((a - b) ** 2).mean()
    psnr = 99.0 if e <= 0 else float(10 * np.log10(255 * 255 / e))

    def opp(x):
        return np.stack([x[:, 0] - x[:, 1], (x[:, 0] + x[:, 1]) / 2 - x[:, 2]], 1)
    oa, ob = opp(a), opp(b)
    ca, cb = np.hypot(*oa.T), np.hypot(*ob.T)
    chroma = float(cb.mean() / ca.mean()) if ca.mean() > 2 else 1.0
    ang = lambda u, v: np.degrees(np.abs(np.angle((u[..., 0] + 1j * u[..., 1]) * np.conj(v[..., 0] + 1j * v[..., 1]))))
    w = np.where(ca > 16, ca * ca, 0)
    hue = float((ang(oa, ob) * w).sum() / w.sum()) if w.sum() > 0 else 0.0
    ma, mb = oa.mean(0), ob.mean(0)
    tint = float(ang(ma, mb)) if np.hypot(*ma) > 3 else 0.0
    luma = np.array([0.299, 0.587, 0.114])  # not a @ luma: BLAS natively, not in the browser (TECH_DEBT build-7)
    la, lb = (x[:, 0] * luma[0] + x[:, 1] * luma[1] + x[:, 2] * luma[2] for x in (a, b))
    contrast = float(lb.std() / la.std()) if la.std() > 1 else 1.0
    return dict(psnr=psnr, chroma=chroma, hue=hue, tint=tint, contrast=contrast)


def _kmeans(px, pal, iters=10):
    """Lloyd refinement of a palette (k,3) on texels (N,3): median cut averages whole boxes, which pulls saturated
    minorities (moss, teal) towards grey."""
    pal = pal.astype(np.float32)
    for _ in range(iters):
        idx = _nearest(px, pal.astype(np.int32))
        for k in range(len(pal)):
            m = idx == k
            if m.any():
                pal[k] = px[m].mean(0)
    return np.clip(pal + 0.5, 0, 255).astype(np.uint8)


def _nearest(rgb, pal):
    """(N,C) colours -> index of the nearest palette entry (N): squared integer distance over the C channels (3 or 4),
    first entry on ties."""
    import geom_native
    L = geom_native.lib()
    if L is not None and len(pal):  # integer distances: the C loop and numpy agree exactly
        x, p = np.asarray(rgb), np.asarray(pal)
        ch = x.shape[-1]  # RGB, or RGBA (the soft path's mip 1 compares alpha too)
        x = np.ascontiguousarray(x.astype(np.int32).reshape(-1, ch))
        p = np.ascontiguousarray(p.astype(np.int32).reshape(-1, ch))
        out = np.empty(len(x), np.int64)
        L.nearest_rgb(len(x), x.ctypes.data, len(p), p.ctypes.data, ch, out.ctypes.data)
        return out
    # int64 squares: add_near marks transparent entries 1e6, whose square overflowed int32 (wrapped, so such an entry
    # could win); same result as before for every in-range palette
    d = ((rgb[:, None, :].astype(np.int32).astype(np.int64) - pal[None, :, :].astype(np.int32)) ** 2).sum(2)
    return d.argmin(1)


def _half(img):
    """RGBA image -> half size (box filter on premultiplied colour), as float RGBA arrays."""
    a = np.asarray(img, np.float32)
    h, w = a.shape[0] // 2, a.shape[1] // 2
    a = a[:h * 2, :w * 2].reshape(h, 2, w, 2, 4)
    al = a[..., 3:4]
    rgb = (a[..., :3] * al).sum((1, 3)) / np.maximum(al.sum((1, 3)), 1e-6)
    return np.concatenate([rgb, al.mean((1, 3))], 2)


def _psnr(src, pal, idx, mask):
    if not mask.any():
        return 99.0
    e = ((src[mask].astype(np.float32) - pal[idx[mask]].astype(np.float32)) ** 2).mean()
    return 99.0 if e <= 0 else 10 * np.log10(255 * 255 / e)


def _pack4(idx):
    idx = idx.astype(np.uint8).reshape(-1)
    return (idx[0::2] | (idx[1::2] << 4)).tobytes()


_LIQ = []


def _liq():
    """tools/liq/liq.py if libimagequant.so is built, else None (kmeans fallback, printed once)"""
    if not _LIQ:
        import os, sys
        d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "liq")
        try:
            if not os.path.exists(os.path.join(d, "libimagequant.so")):
                raise OSError("libimagequant.so not built")
            sys.path.insert(0, d)
            import liq
            liq.lib()
            _LIQ.append(liq)
        except (OSError, ImportError) as e:
            print(f"texmap: TEX_QUANT=liq unavailable ({e}), using kmeans")
            _LIQ.append(None)
    return _LIQ[0]


class Tex(dict):
    """Texture record; t[0], t[1] are w, h like the old (w, h, clut, pixels) tuples (build_chunks reads those)."""
    def __getitem__(self, k):
        return dict.__getitem__(self, ("w", "h")[k] if isinstance(k, int) else k)


_CACHE = []


def _cache():
    """(cache dir, key prefix hash) for encode(), or None: TEX_CACHE (default <main tree>/build/cache/tex, shared like the
    simplify cache; 0 = off). The key covers this file, the liq binding and library, the Pillow / numpy versions, the
    knobs encode reads and the arguments, so a hit is the record encode would return."""
    if not _CACHE:
        import hashlib, os
        d = os.environ.get("TEX_CACHE")
        if d == "0" or os.environ.get("TEX_SHEET"):  # the contact sheet needs encode's REPORT entries
            _CACHE.append(None)
            return None
        if not d:
            import simplify_pool
            d = os.path.join(os.environ.get("DSR_CACHE") or os.path.join(simplify_pool.main_tree(), "build", "cache"),
                             "tex")
        import PIL
        h = hashlib.sha256(b"texmap-cache-1")
        here = os.path.dirname(os.path.abspath(__file__))
        for f in (__file__, os.path.join(here, "liq", "liq.py"), os.path.join(here, "liq", "libimagequant.so")):
            try:
                h.update(open(f, "rb").read())
            except OSError:
                h.update(b"-")
        knobs = [PIL.__version__, np.__version__, QUANT, MIP_CHAIN, sorted(PSM_OVERRIDE.items()),
                 os.environ.get("TEX_FORCE4", ""), os.environ.get("TEX_FORCE8", "")]
        h.update(repr(knobs).encode())
        _CACHE.append((d, h))
    return _CACHE[0]


def encode(w, h, rgba, soft=False, name=""):
    """-> dict(w, h, psm, cpsm, clut, mips=[bytes, bytes], psnr4, psnr8). Results are cached across exports (_cache):
    Undead Burg's blocks share most textures, and a re-export after a non-texture change encodes none."""
    import os, pickle, zlib
    c = _cache()
    if c is None:
        return _encode(w, h, rgba, soft, name)
    k = c[1].copy()
    k.update(repr((w, h, bool(soft), name, len(rgba))).encode())
    k.update(bytes(rgba))
    key = k.hexdigest()
    path = os.path.join(c[0], key[:2], key)
    try:
        with open(path, "rb") as f:
            return Tex(pickle.loads(zlib.decompress(f.read())))
    except (OSError, ValueError, EOFError, pickle.UnpicklingError, zlib.error):
        pass
    t = _encode(w, h, rgba, soft, name)
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = f"{path}.{os.getpid()}.tmp"
        with open(tmp, "wb") as f:
            f.write(zlib.compress(pickle.dumps(dict(t), protocol=pickle.HIGHEST_PROTOCOL), 1))
        os.replace(tmp, path)
    except OSError as e:
        print(f"texmap: cache write failed ({e})")
    return t


def _encode(w, h, rgba, soft=False, name=""):
    from PIL import Image
    img = Image.frombytes("RGBA", (w, h), rgba)
    if w < 128:
        img = img.resize((128, h * 128 // w), Image.BILINEAR)
    if img.size[1] > 128:  # VRAM budget: 128x128 at most
        img = img.resize((img.size[0], 128), Image.BILINEAR)
    w, h = img.size
    half = _half(img)
    if soft:  # blended effects: 256 colours with alpha, CT32 CLUT (alpha 0..0x80)
        import octree  # Pillow's FASTOCTREE, the same on every platform (TECH_DEBT build-7)
        q = octree.quantize(img, 256)
        pal = np.array((list(q.getpalette("RGBA") or []) + [0] * 1024)[:1024], np.uint8).reshape(256, 4)
        clut = bytearray(1024)
        for i in range(256):
            r, g, b, a = pal[i]
            clut[4 * _swz(i):4 * _swz(i) + 4] = bytes([r, g, b, (int(a) * 0x80 + 127) // 255])
        n = max(1, len(q.getpalette("RGBA") or []) // 4)
        m1 = _nearest(np.clip(half.reshape(-1, 4) + 0.5, 0, 255), pal[:n]) if h >= 16 else None
        mips = [q.tobytes()] + ([m1.astype(np.uint8).tobytes()] if m1 is not None else [])
        return Tex(w=w, h=h, psm=PSMT8, cpsm=CT32, clut=bytes(clut), mips=mips, name=name)
    src = np.asarray(img.convert("RGB"), np.uint8).reshape(-1, 3)
    am = np.asarray(img.getchannel("A")).reshape(-1) >= 128
    cut = not am.all()
    out = {}
    for colours in (16, 256):
        n = colours - 1 if cut or colours == 256 else colours  # 8-bit always keeps index 255 transparent (as before)
        liq = _liq() if QUANT == "liq" and am.any() else None
        if liq:
            pal = liq.palette(src[am], n)
            idx = _nearest(src, pal)
        else:
            q = img.convert("RGB").quantize(n, method=Image.Quantize.MEDIANCUT)
            pal = np.array((list(q.getpalette() or []) + [0] * 768)[:3 * n], np.uint8).reshape(-1, 3)[:n]
            pal = pal[:max(1, min(n, len(q.getpalette() or []) // 3))]
            if colours == 16 and am.any():
                sample = src[am][::max(1, am.sum() // 8192)].astype(np.float32)
                pal = _kmeans(sample, pal)
                idx = _nearest(src, pal)
            else:
                idx = np.asarray(q, np.int32).reshape(-1)
        tr = colours - 1
        idx = np.where(am, idx, tr)
        ii = np.minimum(idx[am], len(pal) - 1)
        none = dict(psnr=99.0, chroma=1.0, hue=0.0, tint=0.0, contrast=1.0)
        m32 = _metrics(src[am], pal[ii]) if am.any() else none
        m16 = _metrics(src[am], (_q5(pal) << 3)[ii]) if am.any() else none  # what the GS draws from a CT16 CLUT
        out[colours] = (pal, idx, m32, m16)
    m4, m8 = out[16][2], out[256][2]
    ok4 = m4["psnr"] >= T4_PSNR and m4["chroma"] >= T4_CHROMA and \
        m4["chroma"] >= min(1.0, m8["chroma"]) - T4_CHROMA_REL and m4["hue"] <= m8["hue"] + T4_HUE and \
        m4["tint"] <= m8["tint"] + T4_TINT and m4["contrast"] >= T4_CONTRAST
    force = PSM_OVERRIDE.get(name)
    import os
    for bits in (4, 8):
        if name and name in os.environ.get(f"TEX_FORCE{bits}", "").split(","):
            force = bits
    colours = 16 if (force == 4 or (ok4 and force != 8)) else 256
    pal, idx, m32, m16 = out[colours]
    ct16 = m16["hue"] <= m32["hue"] + CT16_HUE and m16["tint"] <= m32["tint"] + CT16_TINT and \
        m16["chroma"] >= m32["chroma"] - CT16_CHROMA
    REPORT.append(dict(name=name, w=w, h=h, src=src, am=am,
                       cand={c: ((_q5(out[c][0]) << 3) if ct16 and c == colours else out[c][0], out[c][1]) for c in out},
                       m4=m4, m8=m8, m16=m16, rule4=ok4, force=force, chosen=colours, ct16=ct16))
    tr = colours - 1
    # the transparent index's colour: the mean of the opaque texels next to transparent ones (the GS filters CLUT
    # texels bilinearly, so cut-out edges blend towards this colour, not black)
    clear = np.zeros(3, np.uint8)
    if cut:
        a2 = am.reshape(h, w)
        nb = np.zeros_like(a2)
        nb[1:] |= ~a2[:-1]; nb[:-1] |= ~a2[1:]; nb[:, 1:] |= ~a2[:, :-1]; nb[:, :-1] |= ~a2[:, 1:]
        e = (a2 & nb).reshape(-1)
        if e.any():
            clear = pal[idx[e]].mean(0).astype(np.uint8)
    full = np.vstack([pal, np.zeros((colours - len(pal), 3), np.uint8)])
    full[tr] = clear if (cut or colours == 256) else full[tr]
    opaque_n = colours if not cut and colours == 16 else colours - 1
    clut = bytearray((2 if ct16 else 4) * colours)
    for i in range(colours):
        j = _swz(i) if colours == 256 else i
        if ct16:
            struct.pack_into("<H", clut, 2 * j, _ct16(*full[i], i < opaque_n))
        else:
            clut[4 * j:4 * j + 4] = bytes([*full[i], 0x80 if i < opaque_n else 0])
    # mip 1 (and the GS mip chain below it, MIP_CHAIN levels): same palette (opaque entries), transparent where the
    # averaged alpha is below 50 %
    pack = _pack4 if colours == 16 else (lambda x: x.astype(np.uint8).tobytes())
    mips = [pack(idx)]
    lv, lh = half, h // 2
    while lh >= 8 and len(mips) < 2 + MIP_CHAIN:
        hp = lv.reshape(-1, 4)
        m = _nearest(np.clip(hp[:, :3] + 0.5, 0, 255), pal[:opaque_n])
        mips.append(pack(np.where(hp[:, 3] >= 128, m, tr)))
        lv, lh = _half(lv), lh // 2
    return Tex(w=w, h=h, psm=PSMT4 if colours == 16 else PSMT8, cpsm=CT16 if ct16 else CT32, clut=bytes(clut), mips=mips,
               name=name)


def add_near(t, w2, h2, rgba2):
    """Near level for texture record t (opaque / cut-out, from encode): the source at twice mip 0's size, mapped to t's
    palette (texels below 50 % alpha -> the transparent index), packed like mip 0. TECH_DEBT map-2 / texstream-5."""
    from PIL import Image
    img = Image.frombytes("RGBA", (w2, h2), rgba2).resize((2 * t["w"], 2 * t["h"]), Image.BILINEAR)
    n = 16 if t["psm"] == PSMT4 else 256
    pal = np.zeros((n, 3), np.float32)
    for i in range(n):  # palette back from the CLUT (CT16: 5-bit channels as the GS expands them)
        j = _swz(i) if n == 256 else i
        if t["cpsm"] == CT16:
            v = struct.unpack_from("<H", t["clut"], 2 * j)[0]
            pal[i] = [(v & 31) << 3, (v >> 5 & 31) << 3, (v >> 10 & 31) << 3]
            op = bool(v & 0x8000)
        else:
            pal[i] = list(t["clut"][4 * j:4 * j + 3])
            op = t["clut"][4 * j + 3] > 0
        if not op:
            pal[i] = 1e6  # transparent entry: never the nearest colour
    a = np.asarray(img, np.uint8).reshape(-1, 4)
    idx = _nearest(a[:, :3].astype(np.float32), pal)
    idx = np.where(a[:, 3] >= 128, idx, n - 1)
    t["near"] = _pack4(idx) if n == 16 else idx.astype(np.uint8).tobytes()


def densities(textures, chunks):
    """Texels per metre at mip 0 for each texture, area-weighted over the triangles of the chunks using it. Chunks as
    build_chunks returns them: (ti, flags, tris, pos s16x3, st u16x2, colour, min, max, origin, step, st scale)."""
    uv_a, w_a = np.zeros(len(textures)), np.zeros(len(textures))
    for c in chunks:
        ti, ntri, pos, st, org, step, sts = c[0], c[2], c[3], c[4], c[8], c[9], c[10]
        if ti < 0 or not ntri:
            continue
        p = np.frombuffer(bytes(pos), np.int16, ntri * 9).reshape(ntri, 3, 3) * step + np.array(org)
        s = np.frombuffer(bytes(st), np.uint16, ntri * 6).reshape(ntri, 3, 2) / sts
        s = s * np.array([textures[ti]["w"], textures[ti]["h"]])
        w_a[ti] += 0.5 * np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1).sum()
        d1, d2 = s[:, 1] - s[:, 0], s[:, 2] - s[:, 0]
        uv_a[ti] += 0.5 * np.abs(d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]).sum()
    return [float(np.sqrt(u / a)) if a > 0 else 0.0 for u, a in zip(uv_a, w_a)]


def write(blob, base, textures, chunks):
    """Append the texture section to blob (bytearray; base = file offset of blob[0]); returns its file offset."""
    def align(n):
        blob.extend(b"\0" * ((-(len(blob) + base)) % n))
    align(16)
    sec = len(blob) + base
    table = len(blob)
    blob += b"\0" * (ENTRY * len(textures))
    dens = densities(textures, chunks)
    import os
    if os.environ.get("TEX_SHEET"):
        sheet(os.environ["TEX_SHEET"])
    for k, t in enumerate(textures):
        offs = []
        for data in [t["clut"]] + t["mips"]:
            align(16)
            offs.append(len(blob) + base)
            blob += data
        near = 0
        if t.get("near"):
            align(128)
            near = len(blob) + base
            blob += t["near"]
        levels = len(t["mips"])
        w, h = t["w"], t["h"]
        b0 = blocks(t["psm"], w, h)
        b1 = blocks(t["psm"], w // 2, h // 2) if levels > 1 else 0
        struct.pack_into("<2H4BfIIIHHI", blob, table + ENTRY * k, w, h, t["psm"], t["cpsm"], levels, 1 if near else 0,
                         dens[k], offs[0], offs[1], offs[2] if levels > 1 else 0, b0, b1, near)
    return sec


def sheet(path):
    """Contact sheet of the opaque / cut-out textures: source | 8-bit | 4-bit (after the CT16 CLUT, no fog or light),
    the chosen one framed, metrics of the 4-bit version under it. Also writes <path>.txt."""
    from PIL import Image, ImageDraw
    S, cols = 128, 3
    rows = (len(REPORT) + cols - 1) // cols
    cw, ch = 3 * S + 24, S + 34
    img = Image.new("RGB", (cols * cw, rows * ch), (24, 24, 24))
    dr = ImageDraw.Draw(img)
    lines = ["name\tchosen\tct16\trule4\tforce\tpsnr4\tchroma4\thue4\tcontrast4\tpsnr8\tchroma8\thue8\ttint4\ttint8"]
    for k, r in enumerate(REPORT):
        x0, y0 = (k % cols) * cw + 4, (k // cols) * ch + 2
        tiles = [r["src"]] + [r["cand"][c][0][r["cand"][c][1].clip(0, len(r["cand"][c][0]) - 1)] for c in (256, 16)]
        for j, t in enumerate(tiles):
            a = np.where(r["am"][:, None], t, 64).astype(np.uint8).reshape(r["h"], r["w"], 3)
            im = Image.fromarray(a).resize((S, S * r["h"] // r["w"]) if r["w"] >= r["h"] else (S, S))
            img.paste(im, (x0 + j * (S + 6), y0))
        sel = 1 if r["chosen"] == 256 else 2
        dr.rectangle([x0 + sel * (S + 6) - 2, y0 - 2, x0 + sel * (S + 6) + S + 1, y0 + S + 1], outline=(255, 210, 0))
        m = r["m4"]
        dr.text((x0, y0 + S + 3), f"{k} {r['name'][:30]}", fill=(230, 230, 230))
        dr.text((x0, y0 + S + 16), f"T4 {m['psnr']:.1f}dB C{m['chroma']:.2f} H{m['hue']:.1f}/{m['tint']:.1f} K{m['contrast']:.2f}"
                f"{' F' + str(r['force']) if r['force'] else ''}{' CT16' if r['ct16'] else ' CT32'}", fill=(120, 230, 120) if r["rule4"] else (240, 120, 100))
        m8 = r["m8"]
        lines.append(f"{r['name']}\t{4 if r['chosen'] == 16 else 8}\t{int(r['ct16'])}\t{int(r['rule4'])}\t{r['force'] or ''}\t"
                     f"{m['psnr']:.1f}\t{m['chroma']:.3f}\t{m['hue']:.1f}\t{m['contrast']:.3f}\t{m8['psnr']:.1f}\t"
                     f"{m8['chroma']:.3f}\t{m8['hue']:.1f}\t{m['tint']:.1f}\t{m8['tint']:.1f}")
    img.save(path)
    open(path + ".txt", "w").write("\n".join(lines) + "\n")


def stats(textures):
    n4 = sum(t["psm"] == PSMT4 for t in textures)
    ram = sum(len(t["clut"]) + sum(len(m) for m in t["mips"]) + len(t.get("near") or b"") for t in textures)
    nn = sum(1 for t in textures if t.get("near"))
    near_kb = sum(len(t.get("near") or b"") for t in textures) // 1024
    vram0 = sum(blocks(t["psm"], t["w"], t["h"]) for t in textures) * 256
    n16 = sum(t["cpsm"] == CT16 for t in textures)
    return f"{len(textures)} textures, {n4} PSMT4 / {len(textures) - n4} PSMT8, {n16} CT16 CLUTs, {ram // 1024} KB in map.bin (near levels: {nn}, {near_kb} KB), all mip 0 {vram0 // 1024} KB VRAM"


def load(b, ntex, otex):
    """map.bin bytes -> [(w, h, palette (256,4) rgb + alpha 0..0x80, indices (h,w))] at mip 0 (offline previews)."""
    out = []
    for k in range(ntex):
        w, h, psm, cpsm, levels, _, dens, co, p0, p1, b0, b1, _ = struct.unpack_from("<2H4BfIIIHHI", b, otex + ENTRY * k)
        n = 16 if psm == PSMT4 else 256
        pal = np.zeros((256, 4), np.float32)
        for i in range(n):
            j = _swz(i) if n == 256 else i
            if cpsm == CT16:
                v = struct.unpack_from("<H", b, co + 2 * j)[0]
                pal[i] = [(v & 31) << 3, (v >> 5 & 31) << 3, (v >> 10 & 31) << 3, 0x80 if v & 0x8000 else 0]
            else:
                pal[i] = list(b[co + 4 * j:co + 4 * j + 4])
        if psm == PSMT4:
            raw = np.frombuffer(b, np.uint8, w * h // 2, p0)
            ix = np.empty(w * h, np.uint8)
            ix[0::2], ix[1::2] = raw & 15, raw >> 4
        else:
            ix = np.frombuffer(b, np.uint8, w * h, p0)
        out.append((w, h, pal, ix.reshape(h, w)))
    return out
