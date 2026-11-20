"""vu1mapspec.vsm (the map's specular pass, Polaris 2.3) on random R3 batches: positions, fog, near / guard flags and
culling must match vu1map2.vsm's for the same triangles (the pass is drawn over the opaque map with depth GEQUAL); the
colour must match a Python reference of the doc's formula; the indexed entry must kick what the list entry kicks."""
import math, random, struct, sys
from vuemu import VU1, U, F, vertices
import gen
import test_clip as tc
import test_mapi as tm

SRC = tc.SRC
EC, EH, SI = [60.0, 70.0, 80.0], [20.0, 15.0, 10.0], [90.0, 85.0, 70.0]  # 0x80 units


def norm(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def spec_header(rng):
    ls, up = norm([rng.uniform(-1, 1), rng.uniform(0.2, 1), rng.uniform(-1, 1)]), [0.0, 1.0, 0.0]
    v = norm([rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 0)])
    h = norm([ls[i] + v[i] for i in range(3)])
    a, b = [(1, 0), (1.695, -0.695), (3.125, -2.125)][rng.randrange(3)]
    g = rng.uniform(0, 0.5)
    rows = [[ls[i] / 127, 2 * up[i] / 127, v[i] / 127, h[i] / 127] for i in range(3)]
    return dict(ls=ls, up=up, v=v, h=h, a=a, b=b, g=g, rows=rows, cs=[1e6, -sum(v[i] * up[i] for i in range(3)), a, b])


def reference(hd, n, f0):
    """colour of a vertex with normal n (s8 x 127) and F0 f0 (0..127), as the program computes it (float32 aside)"""
    D = [sum(hd['rows'][i][k] * n[i] for i in range(3)) for k in range(4)]
    c = max(min(D[3], D[0] * 1e6), 0)
    d = max(0.0, hd['a'] * c + hd['b']) ** 2
    ry = hd['cs'][1] + D[1] * D[2]
    u = min(1 - D[2], 1.0)
    f = f0 / 127 + hd['g'] * u ** 5
    col = [min(255.0, f * (EC[k] + EH[k] * ry + SI[k] * d)) for k in range(3)]
    return col


def setup(vu, args, cull, hd, nrm, idx=None):
    org, step, K, vs = args
    tc.setup(vu, 'map', args, cull)  # constants 0-7, header 0-6 and the list vertices (pos at 7 + 3 i)
    for k in range(4):
        q = vu.mem[k]; q[3] = q[2]
    top = gen.MAP_IN
    vu.wq(top, [len(vs) // 3, 0x22 if cull else 0, U(hd['g']), 0])
    for k in range(3):
        vu.wq(top + 2 + k, hd['rows'][k], True)
    vu.wq(top + 5, hd['cs'], True)
    vu.wq(top + 6, tc.gif_tag(3, 1, 1, 0x6B, 0, 2) + [0x41, 0])
    for k, c in enumerate((EC + [128.0], EH + [0.0], SI + [0.0], [255.0, 255.0, 255.0, 1 / 127])):
        vu.wq(gen.MAP_SPEC + k, c, True)
    if idx is None:
        for i, (n, f0) in enumerate(nrm):
            vu.wq(top + 7 + 3 * i + 1, [x & 0xFFFFFFFF for x in n] + [f0])
        return
    verts, tris, vn = idx
    H = gen.MAPI_HDR
    start = H + 3 * len(verts)
    vu.wq(top + 7, [len(verts) // 3, start, 0, 0])
    for i, (q, st, col) in enumerate(verts):
        a = top + H + 3 * i
        vu.wq(a, [x & 0xFFFFFFFF for x in q] + [0])
        n, f0 = vn[i]
        vu.wq(a + 1, [x & 0xFFFFFFFF for x in n] + [f0])
        vu.wq(a + 2, [0, 0, 0, 0])
    for t, (a, b, c) in enumerate(tris):
        vu.wq(top + start + t, [a, b, c, 0])


def run(entry, args, cull, hd, nrm, idx=None):
    vu = VU1(open(SRC + 'vu1mapspec.vsm').read())
    setup(vu, args, cull, hd, nrm, idx)
    cyc = vu.run(entry)
    return vu.kicks, cyc


def main(seeds=10, tris=gen.CHUNK_TRIS):
    bad = tot = 0
    cyc = [0, 0, 0]
    for seed in range(seeds):
        rng = random.Random(700 + seed)
        args = tc.make(rng, tris, 0.3)
        org, step, K, vs = args
        vs = list(vs)
        for t in range(1, tris):
            for k in range(rng.randrange(3)):
                vs[3 * t + k] = vs[rng.randrange(3 * t)]
        args = (org, step, K, vs)
        hd = spec_header(rng)
        key = {}
        for q, st, col in vs:  # one normal per distinct corner (the indexed run shares them)
            k = (tuple(q), tuple(st), tuple(col))
            if k not in key:
                n = norm([rng.uniform(-1, 1) for _ in range(3)])
                key[k] = ([int(round(127 * x)) for x in n], rng.randrange(128))
        nrm = [key[(tuple(q), tuple(st), tuple(col))] for q, st, col in vs]
        cull = seed % 2
        # positions / fog / flags / culling: as vu1map2
        vm = VU1(open(SRC + 'vu1map2.vsm').read())
        tc.setup(vm, 'clip', args, cull)
        for k in range(4):
            q = vm.mem[k]; q[3] = q[2]
        c0 = vm.run('VU1Map2_CodeStart')
        ks, c1 = run('VU1MapSpec_CodeStart', args, cull, hd, nrm)
        cyc[0] += c0; cyc[1] += c1
        a, b = vertices(vm.kicks), vertices(ks)
        if len(a) != len(b):
            print('seed %d: %d vertices, vu1map2 %d' % (seed, len(b), len(a)))
            bad += 1
            continue
        for i, (p, s) in enumerate(zip(a, b)):
            tot += 1
            if any(p[k] != s[k] for k in ('x', 'y', 'z', 'f', 'adc')):
                print('seed %d vertex %d: position %s, vu1map2 %s' % (seed, i, s, p))
                bad += 1
                break
            ref = reference(hd, *nrm[i])
            if any(abs(s['rgba'][k] - int(ref[k])) > 1 for k in range(3)) or s['rgba'][3] != 128:
                print('seed %d vertex %d: colour %s, reference %s' % (seed, i, s['rgba'], [round(x, 2) for x in ref]))
                bad += 1
                break
        # indexed entry: the same kicks
        verts, tris_i = tm.index(vs, rng)
        vn = [key[(tuple(q), tuple(st), tuple(col))] for q, st, col in verts]
        ki, c2 = run('VU1MapSpec_Idx', args, cull, hd, nrm, (verts, tris_i, vn))
        cyc[2] += c2
        if ki != ks:
            print('seed %d: indexed kicks differ (%d / %d)' % (seed, sum(x != y for x, y in zip(ki, ks)), len(ks)))
            bad += 1
    print('vertices compared %d, bad %d; cycles vu1map2 %d, spec list %d, spec indexed %d' % (tot, bad, *cyc))
    return bad


if __name__ == '__main__':
    sys.exit(1 if main(int(sys.argv[1]) if len(sys.argv) > 1 else 10) else 0)
