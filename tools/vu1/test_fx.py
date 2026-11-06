"""vu1fx.vsm (VU1 particles, fx.c FX_VU1 builds) against a Python model of fx.c's EE billboard path on random batches:
curve keys and fractions, roll, flipbook cells, the pull toward the camera, fog with edge fog, ADC strip restarts."""
import math, os, random, sys
from vuemu import VU1, U, vertices
import gen
from test_clip import gif_tag, FOC, FOCY, NEAR, FOG, CX, CY

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../ps2/proto/src/')
ZS = 65535 * NEAR  # vu1_map_fog's Z scale (XYZF2)
EDGE = -1.5


def make(rng, n):
    W, H = rng.choice((32, 64, 128)), rng.choice((32, 64, 128))
    fpr, nf = rng.choice(((1, 1), (2, 4), (4, 16), (3, 7), (8, 64)))
    rows = (nf + fpr - 1) // fpr
    systems = []
    for s in range(gen.FX_SYS):
        kx = [rng.uniform(0.2, 3.75) for _ in range(8)]
        ky = [rng.uniform(0.2, 3.75) for _ in range(8)]
        rgba = [[rng.randrange(256) for _ in range(3)] + [rng.randrange(129)] for _ in range(8)]
        systems.append((kx, ky, rgba))
    parts = []
    for i in range(n):
        z = rng.uniform(1.5, 60)
        p = [rng.uniform(-0.6, 0.6) * z, rng.uniform(-0.5, 0.5) * z, z]
        life = rng.uniform(0.3, 4)
        age = rng.uniform(0, life * 0.999)
        sx, sy = rng.uniform(0.01, 0.25), rng.uniform(0.01, 0.25)
        rot = rng.uniform(-math.pi, math.pi)
        fr = rng.randrange(nf)
        parts.append(dict(p=p, t=age / life, sx=sx, sy=sy, rot=rot, fr=fr, sys=rng.randrange(gen.FX_SYS)))
    return dict(W=W, H=H, fpr=fpr, rows=rows, systems=systems, parts=parts)


def setup(vu, b):
    for k in range(4):
        vu.wq(k, [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 0]][k], True)
    vu.wq(4, [FOC, -FOCY, ZS, FOG[0]], True)
    vu.wq(5, [CX, CY, 0, FOG[1]], True)
    for k, c in enumerate(gen.fx_coefs()):
        vu.wq(gen.FX_SC + k, c, True)
    top = vu.top = gen.FX_IN + gen.FX_IN_QW * (vu.batch & 1)
    W, H, fpr, rows = b['W'], b['H'], b['fpr'], b['rows']
    cw, ch, mu, mv = 1 / fpr, 1 / rows, 0.5 / W, 0.5 / H
    du, dv = cw - 2 * mu, ch - 2 * mv
    n = len(b['parts'])
    vu.wq(top, [n, 0, 0, 0])
    vu.wq(top + 1, [cw, ch, 0.5, 1 / fpr], True)
    vu.wq(top + 2, [mu - cw / 2, mv, 0, fpr], True)
    vu.wq(top + 3, [du, dv, 0, EDGE], True)
    vu.wq(top + 4, [du, 0, 0, 0], True)
    vu.wq(top + 5, [0, dv, 0, 0], True)
    vu.wq(top + 6, [FOG[2], FOG[3], 0, 2048], True)
    vu.wq(top + 7, gif_tag(6, 1, 0, 0, 0, 1) + [0xE, 0])
    for k in range(6):
        vu.wq(top + 8 + k, [0x100 + k, 0, 6 + k, 0])
    vu.wq(top + 14, gif_tag(0, 1, 1, 0x7C, 0, 3) + [0x412, 0])
    for s, (kx, ky, rgba) in enumerate(b['systems']):
        for k in range(8):
            a = top + gen.FX_TAB + 16 * s + 2 * k
            vu.wq(a, [kx[k], ky[k], 0, 0], True)
            vu.wq(a + 1, [int(c) for c in rgba[k]])  # V4_8 USN unpack: integers
    for i, p in enumerate(b['parts']):
        a = top + gen.FX_PART + 2 * i
        vu.wq(a, p['p'] + [8 * p['sys'] + min(p['t'] * 7, 6.9999)], True)
        vu.wq(a + 1, [p['sx'], p['sy'], p['rot'], p['fr'] + 0.5], True)


def reference(b):
    """fx.c's EE path (fx_draw, tri_list) for particles it would send to VU1"""
    W, H, fpr, rows = b['W'], b['H'], b['fpr'], b['rows']
    out = []
    for p in b['parts']:
        kx, ky, rgba = b['systems'][p['sys']]
        t = min(p['t'] * 7, 6.9999)
        k0 = int(t)
        f = t - k0
        hx, hy = p['sx'] * (kx[k0] + (kx[k0 + 1] - kx[k0]) * f), p['sy'] * (ky[k0] + (ky[k0 + 1] - ky[k0]) * f)
        col = [rgba[k0][c] + (rgba[k0 + 1][c] - rgba[k0][c]) * f for c in range(4)]
        sr, cr = math.sin(p['rot']), math.cos(p['rot'])
        cw, chh = W / fpr, H / rows
        u0, v0 = (p['fr'] % fpr) * cw, (p['fr'] // fpr) * chh
        c = p['p']
        bias = max(hx, hy)
        kz = (c[2] - bias) / c[2]
        c = [x * kz for x in c]
        hx, hy = hx * kz, hy * kz
        q = 1 / c[2]
        s0 = (CX + FOC * c[0] * q, CY - FOCY * c[1] * q)
        s1 = (FOC * q, -FOCY * q)
        fg = min(max(FOG[0] * c[2] + FOG[1], FOG[2]), FOG[3])
        fe = FOG[1] + EDGE * c[2]
        fg = fg if fg < fe else max(fe, 0)
        for j, (qx, qy) in enumerate(((-1, 1), (1, 1), (-1, -1), (1, -1))):
            ox, oy = (qx * cr - qy * sr) * hx, (qx * sr + qy * cr) * hy
            sx, sy = s0[0] + ox * s1[0], s0[1] + oy * s1[1]
            u = u0 + (cw - 0.5 if qx > 0 else 0.5)
            v = v0 + (0.5 if qy > 0 else chh - 0.5)
            out.append(dict(x=sx * 16, y=sy * 16, z=ZS * q, f=fg, adc=int(j < 2), stq=(u / W, v / H, 1.0),
                            rgba=tuple(int(x) for x in col)))
    return out


def main(seeds=12):
    vu = VU1(open(SRC + 'vu1fx.vsm').read())
    bad = tot = cyc = parts = 0
    worst = [0] * 5
    for seed in range(seeds):
        rng = random.Random(500 + seed)
        n = rng.choice((2, 16, 30, gen.FX_N))
        b = make(rng, n)
        vu.batch = seed
        vu.kicks = []
        vu.vi[8] = 777 if seed == 0 else vu.vi[8]  # garbage slot register first, then whatever the last batch left
        setup(vu, b)
        cyc += vu.run('VU1Fx_CodeStart')
        parts += n
        vs = vertices(vu.kicks[1:])  # kick 0: the A+D header
        ref = reference(b)
        assert len(vs) == len(ref), (len(vs), len(ref))
        for i, (a, r) in enumerate(zip(vs, ref)):
            tot += 1
            d = [abs(((a['x'] - int(r['x'])) + 0x8000) % 0x10000 - 0x8000),
                 abs(((a['y'] - int(r['y'])) + 0x8000) % 0x10000 - 0x8000),
                 abs(a['z'] - int(r['z'])), abs(a['f'] - int(r['f'])),
                 max(abs(x - y) for x, y in zip(a['stq'], r['stq'])) * 4096]
            worst = [max(w, x) for w, x in zip(worst, d)]
            # 1/16 px: 3 (roll polynomial at the largest size); z 1; fog 1; stq 1/4096
            if d[0] > 3 or d[1] > 3 or d[2] > 1 or d[3] > 1 or d[4] > 1 or a['adc'] != r['adc'] or \
                    max(abs(x - y) for x, y in zip(a['rgba'], r['rgba'])) > 1:
                bad += 1
                if bad < 6:
                    print('seed', seed, 'vertex', i, a, r)
    print('worst |dx| %d |dy| %d (1/16 px), z %d, fog %d, stq %.2f / 4096' % tuple(worst))
    print('vertices compared %d, bad %d; %d particles, %.1f cycles per particle incl. batch setup' %
          (tot, bad, parts, cyc / parts))
    return bad


if __name__ == '__main__':
    sys.exit(1 if main(int(sys.argv[1]) if len(sys.argv) > 1 else 12) else 0)
