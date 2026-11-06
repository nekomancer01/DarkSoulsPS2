"""vu1clip.vsm vs a Python reference (near + guard-band Sutherland-Hodgman, fan, screen-space cull) on random map
batches in the R3 format; unclipped triangles are also checked against vu1map.vsm's output."""
import math, os, random, sys
from vuemu import VU1, U, F, vertices
import gen

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ps2/proto/src/')
FOC, FOCY, ZS, NEAR, G = 554.0, 500.0, 65535 * 0.25 * 16, 0.25, 1700.0
FOG = (-30.0, 300.0, 40.0, 255.0)
CX, CY = 2048.0, 2048.0


def gif_tag(nloop, eop, pre, prim, flg, nreg):
    lo = nloop | (eop << 15) | (pre << 46) | (prim << 47) | (flg << 58) | (nreg << 60)
    return [lo & 0xFFFFFFFF, lo >> 32]


def make(rng, tris, near_frac):
    """camera at origin looking +z; triangles around, many crossing z = NEAR or the guard band"""
    org = [rng.uniform(-2, 2), rng.uniform(-1, 1), rng.uniform(0, 3)]
    step = 1 / 1024
    K = 4096.0
    vs = []
    for t in range(tris):
        c = [rng.uniform(-6, 6), rng.uniform(-3, 3), rng.uniform(-1, 8) if rng.random() < near_frac else rng.uniform(2, 20)]
        s = rng.uniform(0.3, 6)
        for k in range(3):
            p = [c[i] + rng.uniform(-s, s) for i in range(3)]
            q = [max(-32768, min(32767, int(round((p[i] - org[i]) / step)))) for i in range(3)]
            st = [rng.randrange(0, 8192), rng.randrange(0, 8192)]
            col = [rng.randrange(256) for _ in range(3)] + [128]
            vs.append((q, st, col))
    return org, step, K, vs


def view_mat():
    # camera basis = identity (right x, up y, forward z), eye at 0: columns 0-3; .w repeats the z row as vu1_begin
    # writes it (view .w = view z)
    return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 1], [0, 0, 0, 0]]


def setup(vu, prog, rng_args, cull):
    org, step, K, vs = rng_args
    tris = len(vs) // 3
    M = view_mat()
    for k in range(4):
        vu.wq(k, M[k], True)
    vu.wq(4, [FOC, -FOCY, ZS, FOG[0]], True); vu.wq(5, [CX, CY, 0, FOG[1]], True)
    vu.wq(6, [FOC / G, FOCY / G, 1, FOG[2]], True); vu.wq(7, [0, 0, -2 * NEAR, FOG[3]], True)
    planes = [[0, 0, 1, -NEAR], [-1, 0, G / FOC, 0], [1, 0, G / FOC, 0], [0, -1, G / FOCY, 0], [0, 1, G / FOCY, 0]]
    for k, pl in enumerate(planes):
        vu.wq(gen.MAP_PLANES + k, pl, True)
    top = gen.MAP_IN
    vu.top = top
    vu.wq(top, [tris, 0x22 if cull else 0, U(K), 0])
    vu.wq(top + 1, org + [step], True)
    clip = prog == 'clip'
    vu.wq(top + 2, gif_tag(3, 1 if clip else 0, 0, 0, 0, 1) + [0xE, 0])
    for k in range(3):
        vu.wq(top + 3 + k, [0x100 + k, 0, 6 + k, 0])
    vu.wq(top + 6, gif_tag(3 if clip else 3 * tris, 1, 1, 0x1B, 0, 3) + [0x412, 0])
    for i, (q, st, col) in enumerate(vs):
        a = top + 7 + 3 * i
        vu.wq(a, [x & 0xFFFFFFFF for x in q] + [0])
        vu.wq(a + 1, st + [0, 0])
        vu.wq(a + 2, col)
    vu.vi[8] = 777  # garbage slot register


def reference(args, cull):
    org, step, K, vs = args
    out = []
    planes = [(0, 0, 1, -NEAR), (-1, 0, G / FOC, 0), (1, 0, G / FOC, 0), (0, -1, G / FOCY, 0), (0, 1, G / FOCY, 0)]
    for t in range(len(vs) // 3):
        poly = []
        for q, st, col in vs[3 * t:3 * t + 3]:
            P = [org[i] + q[i] * step for i in range(3)] + [1.0]
            poly.append(P + [st[0], st[1], K] + [float(c) for c in col])
        dist = lambda v, pl: sum(v[i] * pl[i] for i in range(4))
        # trivial reject per plane / any outside
        if any(all(dist(v, pl) < 0 for v in poly) for pl in planes):
            continue
        if any(dist(v, pl) < 0 for v in poly for pl in planes):
            for pl in planes:
                res = []
                prev = poly[-1]
                for cur in poly:
                    dp, dc = dist(prev, pl), dist(cur, pl)
                    if (dp < 0) != (dc < 0):
                        tt = dp / (dp - dc)
                        res.append([a + (b - a) * tt for a, b in zip(prev, cur)])
                    if dc >= 0:
                        res.append(cur)
                    prev = cur
                poly = res
                if len(poly) < 3:
                    break
            if len(poly) < 3:
                continue
        pv = []
        for v in poly:
            q = 1 / v[2]
            sx, sy, sz = v[0] * q * FOC + CX, -v[1] * q * FOCY + CY, q * ZS
            f = min(max(FOG[0] * v[2] + FOG[1], FOG[2]), FOG[3])
            pv.append(dict(x=int(sx * 16) & 0xFFFF, y=int(sy * 16) & 0xFFFF, z=int(sz * 16) >> 4, f=int(f * 16) >> 4,
                           sx=sx, sy=sy, stq=(v[4] * q, v[5] * q, K * q), rgba=tuple(int(c) for c in v[7:11])))
        for i in range(1, len(pv) - 1):
            a, b, c = pv[0], pv[i], pv[i + 1]
            cr = -((b['sx'] - a['sx']) * (c['sy'] - a['sy']) - (b['sy'] - a['sy']) * (c['sx'] - a['sx']))
            if cull and cr <= 0:
                continue
            out += [a, b, c]
    return out


def main(seeds=10, tris=32):
    bad = tot = 0
    clipped_tris = 0
    for seed in range(seeds):
        rng = random.Random(seed)
        args = make(rng, tris, 0.6)
        cull = seed % 2
        vu = VU1(open(SRC + 'vu1clip.vsm').read())
        setup(vu, 'clip', args, cull)
        snap = [list(q) for q in vu.mem]
        vu.run('VU1Clip_CodeStart')
        got = vertices(vu.kicks)  # triangles kicked with ADC (pass 1 of the two-pass program) are not drawn
        got = [v for t in range(len(got) // 3) if not got[3 * t + 2].get('adc') for v in got[3 * t:3 * t + 3]]
        stray = [a for a in vu.kick_addrs if a not in (gen.MAP_IN + 2, gen.MAP_OUT0, gen.MAP_OUT1)]
        clob = [a for a in list(range(0, 8)) + list(range(gen.MAP_PLANES, 1024)) if vu.mem[a] != snap[a]]
        if stray or clob:
            print('seed %d: kicks outside the slots %s, overwritten %s' % (seed, stray[:5], clob[:8]))
            bad += 1
        ref = reference(args, cull)
        if len(got) != len(ref):
            print('seed %d: %d vertices, reference %d' % (seed, len(got), len(ref)))
            bad += 1
            continue
        # the two-pass program draws the unclipped triangles first: compare triangles in a fixed order
        # (each output triangle against the closest reference triangle still unmatched)
        left, order = list(range(len(ref) // 3)), []
        for t in range(len(got) // 3):
            dist = lambda r: max(abs(got[3 * t + k][c] - ref[3 * r + k][c]) for k in range(3) for c in ('x', 'y'))
            r = min(left, key=dist)
            left.remove(r)
            order.append(r)
        ref = [v for r in order for v in ref[3 * r:3 * r + 3]]
        for a, b in zip(got, ref):
            tot += 1
            d = max(abs(a['x'] - b['x']), abs(a['y'] - b['y']), abs(a['z'] - b['z']) >> 4, abs(a['f'] - b['f']),
                    max(abs(p - q) for p, q in zip(a['rgba'], b['rgba'])))
            d = max(d, max(abs(p - q) / max(1, abs(q)) * 1000 for p, q in zip(a['stq'], b['stq'])))
            if d > 3:
                bad += 1
                if bad < 6:
                    print('seed %d diff %.1f\n got %s\n ref %s' % (seed, d, a, {k: b.get(k) for k in a}))
        print('seed %d: %d triangles out, %d kicks' % (seed, len(got) // 3, len(vu.kicks)))
    print('vertices %d, bad %d' % (tot, bad))
    return bad

if __name__ == '__main__':
    sys.exit(1 if main(int(sys.argv[1]) if len(sys.argv) > 1 else 10) else 0)
