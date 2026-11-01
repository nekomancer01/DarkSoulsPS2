"""vu1skinclip.vsm vs a Python reference (skinning as vu1skin2 + near / guard-band clipping + fan + cull)."""
import math, random, struct, sys
from vuemu import VU1, U, F, vertices
import test_skin as ts, gen

FOC, FOCY, ZS, NEAR, G, FOG, CX, CY = ts.FOC, ts.FOCY, ts.ZS, ts.NEAR, ts.G, ts.FOG, ts.CX, ts.CY
PLANES = [(0, 0, 1, -NEAR), (-1, 0, G / FOC, 0), (1, 0, G / FOC, 0), (0, -1, G / FOCY, 0), (0, 1, G / FOCY, 0)]


def setup(vu, tris, rng, flat, cull, zrange):
    ts.setup(vu, 1, tris, rng, flat, cull, zrange=zrange)   # palette, constants (new style), vertices
    top = 252
    tint = [F(vu.mem[top + 1][k]) for k in range(3)]
    if not flat:
        tint = [t - 64 for t in tint]
    vu.wq(top, [tris, 0x22 if cull else 0, U(0.0 if flat else 1.0), U(255.0)])
    amb, sun = [40, 50, 60], [90, 80, 70]
    if flat:
        vu.wq(top + 1, tint + [128], True); vu.wq(top + 2, [0.5] * 3 + [0], True); vu.wq(top + 3, [0.75] * 3 + [0], True)
    else:
        k = rng.randint(1, 5)
        vu.wq(top + 1, [t + a for t, a in zip(tint, amb)] + [128], True)
        vu.wq(top + 2, [0, 0, 0, 1], True); vu.wq(top + 3, sun + [k], True)  # .w: has m / f, k
        tint = [t + a for t, a in zip(tint, amb)] + sun
        unit = lambda v: [x / math.sqrt(sum(y * y for y in v)) for x in v]
        Ls, Up = unit([rng.uniform(-1, 1) for _ in range(3)]), unit([rng.uniform(-0.3, 0.3), 1, 0.2])
        eu, ed, si = [rng.uniform(60, 240) for _ in range(3)], [rng.uniform(20, 120) for _ in range(3)], \
            [rng.uniform(20, 160) for _ in range(3)]
        q = lambda a: [x / 255 for x in a] + [0]
        blk = [[Ls[i], 2 * Up[i], 0, 0] for i in range(3)] + [[1e6, 0, 0, 0], q([(a + b) / 2 for a, b in zip(eu, ed)]),
               q([(a - b) / 2 for a, b in zip(eu, ed)]), q(si), q(amb), q(sun), q([(a - b) / 4 for a, b in zip(eu, ed)])]
        for i, qw in enumerate(blk):
            vu.wq(gen.SPEC + i, qw, True)
        mf = []
        for i in range(tris * 3):
            a = top + 9 + 4 * i + 3
            mm, ss = rng.choice([0, 0.85, rng.random()]), rng.choice([0, 1, rng.random()])
            vu.mem[a][2], vu.mem[a][3] = round(mm * 255), round((ss * mm + 0.04 * ss * ss * (1 - mm)) * 255)
            mf.append((vu.mem[a][2] / 255, vu.mem[a][3] / 255))
        tint = (tint, dict(Ls=Ls, Up=Up, eu=eu, ed=ed, si=si, amb=amb, sun=sun, k=k, mf=mf))
    vu.wq(top + 8, ts.gif_tag(3, 1, 1, 0x1B, 0, 3) + [0x412, 0])
    for k, pl in enumerate(PLANES):
        vu.wq(gen.MAP_PLANES + k, pl, True)
    vu.vi[8] = 5
    return tint


def reference(vu0, tint, tris, flat, cull):
    m = lambda a: [F(x) for x in vu0.mem[a]]
    s16 = lambda u: (u - (1 << 32) if u & 0x80000000 else u) / 4096.0
    out = []
    top = 252
    for t in range(tris):
        poly = []
        for v in range(3):
            a = top + 9 + 4 * (3 * t + v)
            pa, pb, nm = vu0.mem[a], vu0.mem[a + 1], vu0.mem[a + 2]
            A = [m(8 + pa[3] + k) for k in range(4)]
            B = [m(8 + pb[3] + k) for k in range(4)]
            xa = [sum(A[k][i] * s16(pa[k]) for k in range(3)) + A[3][i] for i in range(3)]
            xb = [sum(B[k][i] * s16(pb[k]) for k in range(3)) + B[3][i] for i in range(3)]
            w = s16(nm[3])
            P = [xb[i] + (xa[i] - xb[i]) * w for i in range(3)] + [1.0]
            nl = max(0.0, sum(A[k][3] * s16(nm[k]) for k in range(3)))
            q3 = vu0.mem[a + 3]
            if flat:
                C = [min(([tint[0], tint[1], tint[2], 128][i]) + q3[i] * (0.5 + 0.75 * nl), [255, 255, 255, 128][i]) for i in range(4)]
            else:  # exact per-vertex specular (vu1-7)
                kt, sp = tint
                dot = lambda x, y: sum(a * b for a, b in zip(x, y))
                n = [sum(A[k][j] * s16(nm[k]) for k in range(3)) for j in range(3)]
                ip = 1 / math.sqrt(dot(P[:3], P[:3]))
                V = [-x * ip for x in P[:3]]
                Hh = [a + b for a, b in zip(sp['Ls'], V)]
                Hh = [x / math.sqrt(dot(Hh, Hh)) for x in Hh]
                c = max(dot(n, Hh), 0) if dot(n, sp['Ls']) > 0 else 0.0
                d = c ** (2 ** sp['k'])
                ry = 2 * dot(n, V) * dot(n, sp['Up']) - dot(V, sp['Up'])
                spc = [(u + b) / 2 + (u - b) / 2 * ry + s * d for u, b, s in zip(sp['eu'], sp['ed'], sp['si'])]
                mm, ff = sp['mf'][3 * t + v]
                C = [min(kt[i] + kt[3 + i] * nl - mm * (sp['amb'][i] + sp['sun'][i] * nl) + ff * spc[i], 255)
                     for i in range(3)] + [128]
            T = [0.0, 0.0, 1.0] if flat else [F(q3[0]), F(q3[1]), 1.0]
            poly.append(P + T + C)
        dist = lambda v, pl: sum(v[i] * pl[i] for i in range(4))
        if any(all(dist(v, pl) < 0 for v in poly) for pl in PLANES):
            continue
        if any(dist(v, pl) < 0 for v in poly for pl in PLANES):
            for pl in PLANES:
                res, prev = [], poly[-1]
                for cur in poly:
                    dp, dc = dist(prev, pl), dist(cur, pl)
                    if (dp < 0) != (dc < 0):
                        tt = dp / (dp - dc)
                        res.append([x + (y - x) * tt for x, y in zip(prev, cur)])
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
                           sx=sx, sy=sy, stq=(v[4] * q, v[5] * q, q), rgba=tuple(int(c) for c in v[7:11])))
        for i in range(1, len(pv) - 1):
            a, b, c = pv[0], pv[i], pv[i + 1]
            cr = -((b['sx'] - a['sx']) * (c['sy'] - a['sy']) - (b['sy'] - a['sy']) * (c['sx'] - a['sx']))
            if cull and cr <= 0:
                continue
            out += [a, b, c]
    return out


def main(seeds=12, tris=12):
    bad = tot = 0
    for seed in range(seeds):
        flat, cull = seed % 2, (seed // 2) % 2
        zr = (-0.5, 4) if seed % 3 else (2, 20)
        vu = VU1(open(ts.SRC + 'vu1skinclip.vsm').read())
        tint = setup(vu, tris, random.Random(seed), flat, cull, zr)
        import copy
        snap = copy.deepcopy(vu)
        vu.run('VU1SkinClip_CodeStart')
        # the program may only kick from its input header (TOPS + 4) or the skin pass's two output slots, and must
        # leave constants, palette and the specular / plane blocks alone (regression: slot toggle used map slots)
        bad_kicks = [a for a in vu.kick_addrs if a not in (256, gen.SKIN_OUT0, gen.SKIN_OUT1)]
        bad_mem = [a for a in list(range(0, 252)) + list(range(992, 1024)) if vu.mem[a] != snap.mem[a]]
        if bad_kicks or bad_mem:
            print('seed %d: kicks outside the slots %s, overwritten %s' % (seed, bad_kicks[:5], bad_mem[:8]))
            bad += 1
        got, ref = vertices(vu.kicks), reference(snap, tint, tris, flat, cull)
        if len(got) != len(ref):
            print('seed %d: %d vs ref %d vertices' % (seed, len(got), len(ref))); bad += 1; continue
        for a, b in zip(got, ref):
            tot += 1
            d = max(abs(a['x'] - b['x']), abs(a['y'] - b['y']), abs(a['z'] - b['z']) >> 4, abs(a['f'] - b['f']),
                    max(abs(p - q) for p, q in zip(a['rgba'], b['rgba'])))
            if not flat:
                d = max(d, max(abs(p - q) / max(1e-6, abs(q)) * 1000 for p, q in zip(a['stq'], b['stq'])))
            if d > 3:
                bad += 1
                if bad < 5: print('seed', seed, 'd', d, a, {k: b[k] for k in a})
        print('seed %d: %d tris out of %d' % (seed, len(got) // 3, tris))
    print('vertices %d bad %d' % (tot, bad))

if __name__ == '__main__':
    main()
