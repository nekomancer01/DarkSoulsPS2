"""Compare ref/vu1skin.vsm (the old in-place skin program, kept only as this test's reference) with vu1skin2.vsm
(pipelined) on random batches in the emulator."""
import math, os, random, struct, sys
from vuemu import VU1, U, vertices

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ps2/proto/src/')
REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ref/')
FOC, FOCY, ZS, NEAR, G = 554.0, 500.0, 65535 * 0.25 * 16, 0.25, 1700.0
FOG = (-30.0, 300.0, 40.0, 255.0)  # a, b, fmin, fmax
CX, CY = 2048.0, 2048.0


def gif_tag(nloop, eop, pre, prim, flg, nreg):
    lo = nloop | (eop << 15) | (pre << 46) | (prim << 47) | (flg << 58) | (nreg << 60)
    return [lo & 0xFFFFFFFF, lo >> 32]


def rot(rng):
    a, b, c = (rng.uniform(0, 2 * math.pi) for _ in range(3))
    ca, sa, cb, sb, cc, sc = math.cos(a), math.sin(a), math.cos(b), math.sin(b), math.cos(c), math.sin(c)
    Rz = [[ca, -sa, 0], [sa, ca, 0], [0, 0, 1]]
    Ry = [[cb, 0, sb], [0, 1, 0], [-sb, 0, cb]]
    Rx = [[1, 0, 0], [0, cc, -sc], [0, sc, cc]]
    mm = lambda A, B: [[sum(A[i][k] * B[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return mm(mm(Rz, Ry), Rx)


def setup(vu, new, tris, rng, flat, cull, bones=20, zrange=(0.1, 12)):
    w = lambda a, v: vu.wq(a, v, True)
    if new:
        w(4, [FOC, -FOCY, ZS, FOG[0]]); w(5, [CX, CY, 0, FOG[1]])
        w(6, [FOC / G, FOCY / G, 2 * NEAR, FOG[2]]); w(7, [0, 0, -1, FOG[3]])
    else:
        w(4, [FOC, -FOCY, ZS, FOG[0]]); w(5, [CX, CY, 0, FOG[1]])
        w(6, [FOC / G, FOCY / G, 1, FOG[2]]); w(7, [0, 0, -2 * NEAR, FOG[3]])
    for b in range(bones):
        R = rot(rng)
        t = [rng.uniform(-3, 3), rng.uniform(-2, 2), rng.uniform(*zrange)]
        L = [rng.uniform(-1, 1) for _ in range(3)]
        n = math.sqrt(sum(x * x for x in L)); L = [x / n for x in L]
        for j in range(3):
            w(8 + 4 * b + j, [R[0][j], R[1][j], R[2][j], L[j]])
        w(8 + 4 * b + 3, t + [0])
    top = 252
    vu.top = top
    tint = [rng.choice([0, 70, 90]), rng.choice([0, 45]), 0]
    vu.wq(top, [tris, 0x22 if cull else 0, 0, 0])
    if new:
        if not flat:
            w(top + 1, [tint[0] + 64, tint[1] + 64, tint[2] + 64, 128]); w(top + 2, [96, 96, 96, 0])
        else:
            w(top + 1, tint + [128]); w(top + 2, [0.5, 0.75, 0, 0])
        w(top + 3, [255, 255, 255, 128])
        vu.wq(top + 4, gif_tag(3, 1, 0, 0, 0, 1) + [0xE, 0])
    else:
        w(top + 1, tint + [128]); w(top + 2, [0.5, 0.75, 0, 255]); w(top + 3, [128, 128, 128, 0])
        vu.wq(top + 4, gif_tag(3, 0, 0, 0, 0, 1) + [0xE, 0])
    for k in range(3):
        vu.wq(top + 5 + k, [0x1234 + k, 0, 0x06 + k, 0])
    n = tris * 3
    if new:
        vu.wq(top + 8, gif_tag(0, 0, 1, 0x1B, 0, 3) + [0x412, 0])
    else:
        vu.wq(top + 8, gif_tag(n, 1, 1, 0x1B, 0, 4) + [0xF412, 0])
    s16 = lambda v: max(-32768, min(32767, int(round(v * 4096)))) & 0xFFFFFFFF
    for i in range(n):
        a = top + 9 + 4 * i
        ba, bb = rng.randrange(bones), rng.randrange(bones)
        vu.wq(a, [s16(rng.uniform(-1, 1)) for _ in range(3)] + [4 * ba])
        vu.wq(a + 1, [s16(rng.uniform(-1, 1)) for _ in range(3)] + [4 * bb])
        nv = [rng.uniform(-1, 1) for _ in range(3)]
        nn = math.sqrt(sum(x * x for x in nv)); nv = [x / nn for x in nv]
        wa = rng.choice([1.0, 1.0, rng.random()])
        vu.wq(a + 2, [s16(x) for x in nv] + [s16(wa)])
        if flat:
            vu.wq(a + 3, [rng.randrange(256), rng.randrange(256), rng.randrange(256), 0x80])
        else:
            vu.wq(a + 3, [U(rng.random()), U(rng.random()), 0, 0])


def run(path, entry, new, tris, seed, flat, cull, zrange):
    vu = VU1(open(path).read())
    rng = random.Random(seed)
    setup(vu, new, tris, rng, flat, cull, zrange=zrange)
    cyc = vu.run(entry)
    return vertices(vu.kicks), cyc, vu


def compare(seeds=20, tris=24):
    bad = tot = adc_mis = 0
    worst = 0
    cyc_o = cyc_n = 0
    for seed in range(seeds):
        flat, cull = seed % 2, (seed // 2) % 2
        zr = (0.1, 12) if seed % 3 else (2, 30)
        vo, co, _ = run(REF + 'vu1skin.vsm', 'VU1Skin_Flat' if flat else 'VU1Skin_Tex', 0, tris, seed, flat, cull, zr)
        vn, cn, _ = run(SRC + 'vu1skin2.vsm', 'VU1Skin2_Flat' if flat else 'VU1Skin2_Tex', 1, tris, seed, flat, cull, zr)
        cyc_o += co; cyc_n += cn
        assert len(vo) == len(vn) == tris * 3, (len(vo), len(vn))
        for i, (a, b) in enumerate(zip(vo, vn)):
            tot += 1
            if i % 3 == 2 and a['adc'] != b['adc']:
                adc_mis += 1
            tri_drawn = not a['adc'] if i % 3 == 2 else True
            d = max(abs(a['x'] - b['x']), abs(a['y'] - b['y']), abs(a['z'] - b['z']) >> 4, abs(a['f'] - b['f']),
                    max(abs(p - q) for p, q in zip(a['rgba'], b['rgba'])))
            if not flat:
                d = max(d, max(abs(p - q) * 1000 for p, q in zip(a['stq'], b['stq'])))
            if d > 2:
                bad += 1
                if bad <= 5:
                    print('seed %d v %d: old %s\n            new %s' % (seed, i, a, b))
    print('vertices %d, differing (>2 units) %d, triangle ADC mismatches %d; cycles old %d new %d (%.2fx)' %
          (tot, bad, adc_mis, cyc_o, cyc_n, cyc_o / cyc_n))
    return bad + adc_mis

if __name__ == '__main__':
    sys.exit(1 if compare(int(sys.argv[1]) if len(sys.argv) > 1 else 12) else 0)
