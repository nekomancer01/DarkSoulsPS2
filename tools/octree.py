"""Pillow's FASTOCTREE quantizer (Image.quantize(method=Quantize.FASTOCTREE), libImaging/QuantOctree.c) in numpy, so
the result does not depend on the platform's qsort.

Why (TECH_DEBT build-7): QuantOctree.c sorts its colour buckets by count with qsort, and many buckets tie (every empty
one, and equal counts). glibc's qsort and the musl one in Pyodide (the in-browser converter) leave ties in different
orders, so the same RGBA image gave different palettes natively and in the browser, in every Pillow version tried
(10.2 / 11.3 in Pyodide; 11.0 to 12.3 natively all agree with each other). Here ties keep the cube order (a stable sort),
which is what the native runs give.

Kept from QuantOctree.c / Quant.c, because the exports' palettes so far came from them: averages are float32 sum /
count truncated to int, and transparent pixels after the first take its RGB.

    quantize(img, colors=256) -> PIL "P" image with an RGBA (or RGB) palette, used like img.quantize's result
"""
import numpy as np

CUBE = (4, 4, 4, 0, 2, 2, 2, 0)        # fine r g b a bits, coarse r g b a bits (CUBE_LEVELS)
CUBE_ALPHA = (3, 4, 3, 3, 2, 2, 2, 2)  # CUBE_LEVELS_ALPHA


def _offsets(bits):
    """(rOffset, gOffset, bOffset, aOffset) of new_color_cube"""
    r, g, b, a = bits
    return g + b + a, b + a, a, 0


def _index(px, bits):
    """color_bucket_offset for (n, 4) int64 pixels"""
    o = _offsets(bits)
    k = np.zeros(len(px), np.int64)
    for c in range(4):
        k |= (px[:, c] >> (8 - bits[c])) << o[c]
    return k.astype(np.intp)  # np.bincount takes no int64 under the browser's 32-bit numpy


def _avg(cnt, sums):
    """avg_color_from_color_bucket: (int)(float32(sum) / float32(count)), clipped; zero where count == 0"""
    c = cnt.astype(np.float32)
    with np.errstate(divide="ignore", invalid="ignore"):
        v = sums.astype(np.float32) / np.where(c != 0, c, np.float32(1))[:, None]
    v = np.clip(v.astype(np.int64), 0, 255)  # (int) truncates toward zero; values are >= 0
    v[cnt == 0] = 0
    return v


def _copy(cnt, sums, src_bits, dst_bits):
    """copy_color_cube: every bucket of the larger cube added to its bucket in the other"""
    width = [1 << max(s, d) for s, d in zip(src_bits, dst_bits)]
    g = np.indices(width).reshape(4, -1).T.astype(np.int64)  # (r, g, b, a) in the loops' order
    so, do = _offsets(src_bits), _offsets(dst_bits)
    src = np.zeros(len(g), np.int64)
    dst = np.zeros(len(g), np.int64)
    for c in range(4):
        src |= (g[:, c] >> max(0, dst_bits[c] - src_bits[c])) << so[c]
        dst |= (g[:, c] >> max(0, src_bits[c] - dst_bits[c])) << do[c]
    src, dst = src.astype(np.intp), dst.astype(np.intp)
    size = 1 << sum(dst_bits)
    ocnt = np.bincount(dst, weights=cnt[src], minlength=size).astype(np.int64)
    osum = np.stack([np.bincount(dst, weights=sums[src, c], minlength=size) for c in range(4)], 1).astype(np.int64)
    return ocnt, osum


def _sorted(cnt):
    """create_sorted_color_palette's order: count descending, ties in cube order"""
    return np.argsort(-cnt, kind="stable")


def quantize_rgba(px, colors=256, alpha=True):
    """px: (n, 4) uint8 pixels -> (indices (n,) uint8, palette (colors, 4) uint8), as quantize_octree"""
    px = np.asarray(px, np.int64).reshape(-1, 4).copy()
    if alpha:  # Quant.c: every transparent pixel after the first takes the first one's RGB
        t = np.nonzero(px[:, 3] == 0)[0]
        if len(t) > 1:
            px[t[1:], :3] = px[t[0], :3]
    else:
        px[:, 3] = 255
    cb = CUBE_ALPHA if alpha else CUBE
    fb, kb = cb[:4], cb[4:]
    fk = _index(px, fb)
    fsize = 1 << sum(fb)
    fcnt = np.bincount(fk, minlength=fsize).astype(np.int64)
    fsum = np.stack([np.bincount(fk, weights=px[:, c], minlength=fsize) for c in range(4)], 1).astype(np.int64)
    ccnt, csum = _copy(fcnt, fsum, fb, kb)
    n_coarse = min(int((ccnt > 0).sum()), colors)
    n_fine = colors - n_coarse
    order = _sorted(fcnt)
    fine_cnt, fine_sum = fcnt[order], fsum[order]

    def subtract(lo, hi):  # subtract_color_buckets
        for i in range(lo, hi):
            if fine_cnt[i] == 0:
                continue
            k = _index(_avg(fine_cnt[i:i + 1], fine_sum[i:i + 1]), kb)[0]
            ccnt[k] -= fine_cnt[i]
            csum[k] -= fine_sum[i]
    subtract(0, n_fine)
    while n_coarse > int((ccnt > 0).sum()):
        done = n_fine
        n_coarse = int((ccnt > 0).sum())
        n_fine = colors - n_coarse
        subtract(done, n_fine)
    corder = _sorted(ccnt)
    pal_cnt = np.concatenate([ccnt[corder][:n_coarse], fine_cnt[:n_fine]])
    pal_sum = np.concatenate([csum[corder][:n_coarse], fine_sum[:n_fine]])
    pal = _avg(pal_cnt, pal_sum)
    # lookup: the coarse cube holds the coarse palette indices (lowest index wins), expanded to the fine cube, then the
    # fine palette entries written over it (lowest index wins)
    lk = np.zeros(1 << sum(kb), np.int64)
    for i in range(n_coarse - 1, -1, -1):
        lk[_index(pal[i:i + 1], kb)[0]] = i
    lf, _ = _copy(lk, np.zeros((len(lk), 4), np.int64), kb, fb)
    for i in range(n_coarse + n_fine - 1, n_coarse - 1, -1):
        lf[_index(pal[i:i + 1], fb)[0]] = i
    idx = lf[fk] & 0xFF  # stored as unsigned char
    return idx.astype(np.uint8), pal.astype(np.uint8)


def quantize(img, colors=256):
    """img.quantize(colors, method=Quantize.FASTOCTREE) for an RGB / RGBA PIL image, platform independent"""
    from PIL import Image
    alpha = img.mode == "RGBA"
    px = np.asarray(img.convert("RGBA") if not alpha else img, np.uint8).reshape(-1, 4)
    idx, pal = quantize_rgba(px, colors, alpha)
    q = Image.frombytes("P", img.size, idx.tobytes())
    q.putpalette(pal.tobytes() if alpha else pal[:, :3].tobytes(), "RGBA" if alpha else "RGB")
    return q
