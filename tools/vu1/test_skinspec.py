"""vu1skin2.vsm VU1Skin2_TexSpec (cheap specular, gen.py spec_stages) on character-like batches (bones within 0.6 m of a
centre 3..10 m away):
  1. against a model of the same formula (per-draw V, per-batch power fit): catches code bugs, expect ~0;
  2. against the exact per-vertex reference of visuals-fog-light's spec (V = -p/|p|, H per vertex, exact (n.H)^(2^k)):
     the approximation error, reported as max / mean colour difference (0..255);
  3. m = f = 0: TexSpec colours bit-identical to the Tex entry (and to ref/vu1skin2_nospec.vsm, pre-specular)."""
import math, random, sys, copy
from vuemu import VU1, U, F, vertices
import test_skin as ts, gen

BIG = 1e6
import os
ZR = tuple(float(x) for x in os.environ.get("SPEC_ZR", "3,10").split(","))  # character centre distance (m)
POW = {1: (1.0, 0.0), 2: (1.695, -0.695), 3: (3.125, -2.125), 4: (5.99, -4.99), 5: (11.73, -10.73)}  # = vu1.c SPEC_POW


def unit(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v]


def setup(vu, rng, tris, cull, zero):
    ts.setup(vu, 1, tris, rng, 0, cull, zrange=(2, 12))
    centre = [rng.uniform(-1.5, 1.5), rng.uniform(-1, 1), rng.uniform(*ZR)]
    for b in range(20):  # character-like: bones near one centre
        q = vu.mem[8 + 4 * b + 3]
        vu.wq(8 + 4 * b + 3, [centre[i] + rng.uniform(-0.6, 0.6) for i in range(3)] + [0], True)
    Ls, Up = unit([rng.uniform(-1, 1) for _ in range(3)]), unit([rng.uniform(-0.3, 0.3), 1, rng.uniform(-0.3, 0.3)])
    Vc = unit([-x for x in centre])
    Hc = unit([a + b for a, b in zip(Ls, Vc)])
    eu = [rng.uniform(60, 240) for _ in range(3)]
    ed = [rng.uniform(20, 120) for _ in range(3)]
    si = [rng.uniform(20, 160) for _ in range(3)]
    amb = [rng.uniform(20, 80) for _ in range(3)]
    sun = [rng.uniform(20, 120) for _ in range(3)]
    k = rng.randint(1, 5)
    q = lambda a: [x / 255 for x in a] + [0]
    blk = [[Ls[i], 2 * Up[i], Vc[i], Hc[i]] for i in range(3)] + [
        [BIG, -sum(a * b for a, b in zip(Vc, Up)), 0, 0], q([(a + b) / 2 for a, b in zip(eu, ed)]),
        q([(a - b) / 2 for a, b in zip(eu, ed)]), q(si), q(amb), q(sun)]
    for i, qw in enumerate(blk):
        vu.wq(gen.SPEC + i, qw, True)
    top = 252
    tint = [F(vu.mem[top + 1][c]) - 64 for c in range(3)]
    k1 = [t + a for t, a in zip(tint, amb)]
    vu.wq(top + 1, k1 + [128], True)
    vu.wq(top + 2, sun + [0], True)
    vu.mem[top][2], vu.mem[top][3] = U(POW[k][0]), U(POW[k][1])
    ms = []
    for i in range(tris * 3):
        a = top + 9 + 4 * i + 3
        m, s = (0, 0) if zero else (rng.choice([0, 0.85, rng.random()]), rng.choice([0, 1, rng.random()]))
        f = s * m + 0.04 * s * s * (1 - m)
        vu.mem[a][2], vu.mem[a][3] = round(m * 255), round(f * 255)
        ms.append((round(m * 255) / 255, round(f * 255) / 255))
    return dict(Ls=Ls, Up=Up, Vc=Vc, Hc=Hc, eu=eu, ed=ed, si=si, amb=amb, sun=sun, k=k), k1, ms


def references(vu0, sp, k1, ms, tris):
    s16 = lambda u: (u - (1 << 32) if u & 0x80000000 else u) / 4096.0
    m4 = lambda a: [F(x) for x in vu0.mem[a]]
    dot = lambda x, y: sum(a * b for a, b in zip(x, y))
    model, exact = [], []
    a_, b_ = POW[sp['k']]
    for i in range(tris * 3):
        a = 252 + 9 + 4 * i
        pa, pb, nm = vu0.mem[a], vu0.mem[a + 1], vu0.mem[a + 2]
        A = [m4(8 + pa[3] + k) for k in range(4)]
        B = [m4(8 + pb[3] + k) for k in range(4)]
        xa = [sum(A[k][j] * s16(pa[k]) for k in range(3)) + A[3][j] for j in range(3)]
        xb = [sum(B[k][j] * s16(pb[k]) for k in range(3)) + B[3][j] for j in range(3)]
        w = s16(nm[3])
        p = [xb[j] + (xa[j] - xb[j]) * w for j in range(3)]
        nb = [s16(nm[k]) for k in range(3)]
        n = [sum(A[k][j] * nb[k] for k in range(3)) for j in range(3)]
        nl = max(0.0, sum(A[k][3] * nb[k] for k in range(3)))
        m, f = ms[i]
        Ls, Up = sp['Ls'], sp['Up']
        for V, out, cheap in ((sp['Vc'], model, True), (unit([-x for x in p]), exact, False)):
            H = sp['Hc'] if cheap else unit([a + b for a, b in zip(Ls, V)])
            c = max(dot(n, H), 0) if dot(n, Ls) > 0 else 0.0
            if cheap:
                c = min(max(dot(n, H), 0), max(dot(n, Ls) * BIG, 0))
                d = max(0, a_ * c + b_) ** 2
            else:
                d = c ** (2 ** sp['k'])
            ry = 2 * dot(n, V) * dot(n, Up) - dot(V, Up)
            t = ry * 0.5 + 0.5 if cheap else min(max(ry * 0.5 + 0.5, 0), 1)
            env = [b + (u - b) * t for u, b in zip(sp['eu'], sp['ed'])]
            spc = [e + s * d for e, s in zip(env, sp['si'])]
            col = [min(k1[j] + sp['sun'][j] * nl - m * (sp['amb'][j] + sp['sun'][j] * nl) + f * spc[j], 255)
                   for j in range(3)]
            out.append([int(max(x, 0)) for x in col] + [128])
    return model, exact


def run(path, entry, seed, tris, zero):
    rng = random.Random(seed)
    vu = VU1(open(path).read())
    sp, k1, ms = setup(vu, rng, tris, seed % 2, zero)
    snap = copy.deepcopy(vu)
    cyc = vu.run(entry)
    return vertices(vu.kicks), snap, sp, k1, ms, cyc


def main(seeds=10, tris=24):
    prog = ts.SRC + 'vu1skin2.vsm'
    mb = eb = 0
    errs = []
    n = 0
    for seed in range(seeds):
        got, snap, sp, k1, ms, cyc = run(prog, 'VU1Skin2_TexSpec', seed, tris, False)
        model, exact = references(snap, sp, k1, ms, tris)
        for g, r, x in zip(got, model, exact):
            n += 1
            dm = max(abs(a - b) for a, b in zip(g['rgba'], r))
            mb += dm > 2
            errs.append(max(abs(a - b) for a, b in zip(g['rgba'], x)))
    errs.sort()
    print('TexSpec vs model of its formula: %d of %d vertices off by > 2' % (mb, n))
    print('TexSpec vs exact per-vertex Blinn: colour error max %d, mean %.2f, 95th percentile %d (0..255)' %
          (errs[-1], sum(errs) / len(errs), errs[int(len(errs) * 0.95)]))
    diff = 0
    others = [(prog, 'VU1Skin2_Tex')] + ([(sys.argv[1], 'VU1Skin2_Tex')] if len(sys.argv) > 1 else [])
    for seed in range(seeds):
        base = run(prog, 'VU1Skin2_TexSpec', seed, tris, True)[0]
        for path, entry in others:
            diff += sum(a != b for a, b in zip(base, run(path, entry, seed, tris, True)[0]))
    print('m = f = 0: %d vertices differ from %s' % (diff, ', '.join(p.split('/')[-1] + ':' + e for p, e in others)))
    return mb + diff


if __name__ == '__main__':
    sys.exit(1 if main() else 0)
