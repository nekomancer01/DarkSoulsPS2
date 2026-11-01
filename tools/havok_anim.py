"""Havok skeleton + hkaSplineCompressedAnimation decoding (DSR, Havok 2015 tagfiles).
Spline layout and quaternion packings ported from SoulsAssetPipeline's SplineCompressedAnimation.cs."""
import math, struct

import souls


def skeleton(hkx):
    """-> (bone names, parent indices, reference pose [(t xyz, q xyzw, s xyz)])"""
    h = souls.HkTag(hkx)
    T, B = h.type("hkaSkeleton"), h.type("hkaBone")
    o = h.objects("hkaSkeleton")[0]
    _, po, pc = h.item(o, T, "parentIndices")
    parents = list(struct.unpack_from(f"<{pc}h", h.data, po))
    _, bo, bc = h.item(o, T, "bones")
    names = []
    for i in range(bc):
        _, so, sc = h.items[struct.unpack_from("<I", h.data, bo + i * B.size + B.field("name")[0])[0]]
        names.append(h.data[so:so + sc].split(b"\0")[0].decode())
    _, ro, rc = h.item(o, T, "referencePose")
    ref = []
    for i in range(rc):
        v = struct.unpack_from("<12f", h.data, ro + 48 * i)
        ref.append((v[0:3], v[4:8], v[8:11]))
    return names, parents, ref


class _R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def pad(self, n):
        self.p = (self.p + n - 1) // n * n

    def u8(self):
        self.p += 1
        return self.b[self.p - 1]

    def un(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.p)
        self.p += struct.calcsize(fmt)
        return v if len(v) > 1 else v[0]


ROT_ALIGN = {0: 4, 1: 1, 2: 2, 3: 1, 4: 2, 5: 4}


def _quat(r, qt):
    if qt == 0:  # POLAR32
        c = r.un("I")
        rr = ((c >> 18) & 0x3FF) / 1023.0
        rr = 1.0 - rr * rr
        pt = c & 0x3FFFF
        phi = math.floor(math.sqrt(pt))
        theta = 0.0
        if phi > 0:
            theta = (math.pi / 4) * (pt - phi * phi) / phi
            phi = (math.pi / 2 / 511.0) * phi
        mag = math.sqrt(max(0.0, 1.0 - rr * rr))
        q = [math.sin(phi) * math.cos(theta) * mag, math.sin(phi) * math.sin(theta) * mag, math.cos(phi) * mag, rr]
        for k, bit in enumerate((0x10000000, 0x20000000, 0x40000000, 0x80000000)):
            if c & bit:
                q[k] = -q[k]
        return tuple(q)
    if qt in (1, 2):
        if qt == 1:  # THREECOMP40
            c = int.from_bytes(r.b[r.p:r.p + 5], "little")
            r.p += 5
            mask, frac = (1 << 12) - 1, 0.000345436
            vals = [(c & mask), (c >> 12) & mask, (c >> 24) & mask]
            shift, neg = (c >> 36) & 3, (c >> 38) & 1
            vals = [v - (mask >> 1) for v in vals]
        else:  # THREECOMP48
            x, y, z = r.un("hhh")
            mask, frac = (1 << 15) - 1, 0.000043161
            shift = ((y >> 14) & 2) | ((x >> 15) & 1)
            neg = (z >> 15) != 0
            vals = [(v & mask) - (mask >> 1) for v in (x, y, z)]
        t = [v * frac for v in vals]
        q, j = [0.0] * 4, 0
        for i in range(4):
            if i != shift:
                q[i] = t[j]
                j += 1
        w = 1.0 - t[0] * t[0] - t[1] * t[1] - t[2] * t[2]
        q[shift] = math.sqrt(w) if w > 0 else 0.0
        if neg:
            q[shift] = -q[shift]
        return tuple(q)
    if qt == 5:
        return r.un("4f")
    raise NotImplementedError(f"rotation quantization {qt}")


def _scalar(r, lo, hi, qt):
    v = r.u8() / 255.0 if qt == 0 else r.un("H") / 65535.0
    return lo + (hi - lo) * v


def _knot_span(degree, frame, n, knots):
    if frame >= knots[n]:
        return n - 1
    low, high = degree, n
    mid = (low + high) // 2
    while frame < knots[mid] or frame >= knots[mid + 1]:
        if frame < knots[mid]:
            high = mid
        else:
            low = mid
        mid = (low + high) // 2
    return mid


def _basis(span, degree, frame, knots):
    N = [1.0, 0, 0, 0, 0]
    for i in range(1, degree + 1):
        for j in range(i - 1, -1, -1):
            a = (frame - knots[span - j]) / (knots[span + i - j] - knots[span - j])
            tmp = N[j] * a
            N[j + 1] += N[j] - tmp
            N[j] = tmp
    return N


class _Spline:
    """B-spline over knots; values are scalars or 4-tuples"""

    def __init__(self, degree, knots, values):
        self.degree, self.knots, self.values = degree, knots, values

    def at(self, frame):
        n = len(self.values)
        span = _knot_span(self.degree, frame, n, self.knots)
        N = _basis(span, self.degree, frame, self.knots)
        if isinstance(self.values[0], tuple):
            out = [0.0] * 4
            for i in range(self.degree + 1):
                v = self.values[span - i]
                for k in range(4):
                    out[k] += v[k] * N[i]
            return tuple(out)
        return sum(self.values[span - i] * N[i] for i in range(self.degree + 1))


def _knots(r):
    n, deg = r.un("h"), r.u8()
    return n, deg, list(r.b[r.p:r.p + n + deg + 2]), n + deg + 2


def _vec_track(r, types, qt, default):
    """Position/scale: per axis None (default), static float, or spline. types: bits 0-2 static, 4-6 spline."""
    spline = types & 0x70
    if spline:
        n, deg, knots, kc = _knots(r)
        r.p += kc
        r.pad(4)
        bounds, out = {}, [None, None, None]
        for ax in range(3):
            if types & (0x10 << ax):
                bounds[ax] = r.un("ff")
            elif types & (1 << ax):
                out[ax] = r.un("f")
        vals = {ax: [] for ax in bounds}
        for _ in range(n + 1):
            for ax in range(3):
                if ax in bounds:
                    vals[ax].append(_scalar(r, *bounds[ax], qt))
        for ax in bounds:
            out[ax] = _Spline(deg, knots, vals[ax])
        return [default if v is None else v for v in out]
    out = []
    for ax in range(3):
        out.append(r.un("f") if types & (1 << ax) else default)
    return out


def _block(data, ntracks):
    r = _R(data)
    masks = [(r.u8(), r.u8(), r.u8(), r.u8()) for _ in range(ntracks)]
    r.pad(4)
    tracks = []
    for q, pos_t, rot_t, scl_t in masks:
        pq, rq, sq = q & 3, (q >> 2) & 0xF, (q >> 6) & 3
        pos = _vec_track(r, pos_t, pq, 0.0)
        r.pad(4)
        if rot_t & 0xF0:
            n, deg, knots, kc = _knots(r)
            r.p += kc
            r.pad(ROT_ALIGN[rq])
            rot = _Spline(deg, knots, [_quat(r, rq) for _ in range(n + 1)])
        elif rot_t & 0x0F:
            r.pad(ROT_ALIGN[rq])
            rot = _quat(r, rq)
        else:
            rot = None
        r.pad(4)
        scl = _vec_track(r, scl_t, sq, 1.0)
        r.pad(4)
        tracks.append((pos, rot, scl))
    return tracks


def _ev(v, f):
    return v.at(f) if isinstance(v, _Spline) else v


def animation(hkx):
    """-> (frame count, fps, track->bone list, frames: [[(t xyz, q xyzw, s xyz) per track]])"""
    h = souls.HkTag(hkx)
    A = h.type("hkaSplineCompressedAnimation")
    o = h.objects("hkaSplineCompressedAnimation")[0]
    nframes, nblocks = h.get(o, A, "numFrames", "i"), h.get(o, A, "numBlocks", "i")
    per_block = h.get(o, A, "maxFramesPerBlock", "i")
    ntracks = h.get(o, A, "numberOfTransformTracks", "i")
    fps = 1.0 / h.get(o, A, "frameDuration", "f")
    _, do, dc = h.item(o, A, "data")
    _, bo, bc = h.item(o, A, "blockOffsets")
    data = h.data[do:do + dc]
    offs = list(struct.unpack_from(f"<{bc}I", h.data, bo)) + [dc]
    blocks = [_block(data[offs[i]:offs[i + 1]], ntracks) for i in range(nblocks)]
    Bd = h.type("hkaAnimationBinding")
    bo2 = h.objects("hkaAnimationBinding")[0]
    _, to, tc = h.item(bo2, Bd, "transformTrackToBoneIndices")
    t2b = list(struct.unpack_from(f"<{tc}h", h.data, to)) if tc else list(range(ntracks))
    frames = []
    for f in range(nframes):
        blk = min(f // (per_block - 1) if nblocks > 1 else 0, nblocks - 1)
        lf = float(f - blk * (per_block - 1)) if nblocks > 1 else float(f)
        row = []
        for pos, rot, scl in blocks[blk]:
            q = _ev(rot, lf) if rot is not None else (0.0, 0.0, 0.0, 1.0)
            ql = math.sqrt(sum(c * c for c in q)) or 1.0
            row.append((tuple(_ev(c, lf) for c in pos), tuple(c / ql for c in q), tuple(_ev(c, lf) for c in scl)))
        frames.append(row)
    return nframes, fps, t2b, frames


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + y * tz - z * ty, vy + w * ty + z * tx - x * tz, vz + w * tz + x * ty - y * tx)


def world(parents, local):
    """Local (t, q, s) per bone -> world (t, q) per bone (scale ignored: DSR bones are unscaled)."""
    out = []
    for i, (t, q, _) in enumerate(local):
        p = parents[i]
        if p < 0:
            out.append((t, q))
        else:
            pt, pq = out[p]
            rt = qrot(pq, t)
            out.append(((pt[0] + rt[0], pt[1] + rt[1], pt[2] + rt[2]), qmul(pq, q)))
    return out
