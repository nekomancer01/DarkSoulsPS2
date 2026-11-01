"""vu1map2.vsm edge fog loop (header 0.w != 0, render-distance): every output equals the plain loop's except the fog
factor, which must be max(min(clamp(a z + b, fmin, fmax), b + a2 z), 0) (a, b, fmin, fmax = test_clip.FOG)."""
import random, struct, sys
from vuemu import VU1, vertices
import test_clip as tc

def run(args, cull, a2):
    vu = VU1(open(tc.SRC + 'vu1map2.vsm').read())
    tc.setup(vu, 'clip', args, cull)
    for k in range(4):  # matrix .w row = z row (view .w = view z)
        q = vu.mem[k]; q[3] = q[2]
    if a2:
        u = struct.unpack('<I', struct.pack('<f', a2))[0] | 1
        vu.mem[vu.top][3] = u
    cyc = vu.run('VU1Map2_CodeStart')
    return vertices(vu.kicks), cyc

def main(seeds=10, tris=54, a2=-20.0):
    a, b, fmin, fmax = tc.FOG
    bad = tot = edged = 0
    c0 = c1 = 0
    for seed in range(seeds):
        rng = random.Random(300 + seed)
        args = tc.make(rng, tris, 0.0)
        org, step, K, vs = args
        v0, cy0 = run(args, seed % 2, 0)
        v1, cy1 = run(args, seed % 2, a2)
        c0 += cy0; c1 += cy1
        assert len(v0) == len(v1), (len(v0), len(v1))
        # output order = input order for unculled triangles; match by screen position instead of index
        zs = {}
        for q, st, col in vs:
            zs.setdefault((q[0], q[1]), []).append(org[2] + q[2] * step)
        for i, (p, q) in enumerate(zip(v0, v1)):
            if p['adc'] != q['adc'] or (p['x'], p['y'], p['z'], p['rgba'], p['stq']) != \
                    (q['x'], q['y'], q['z'], q['rgba'], q['stq']):
                bad += 1; print('seed', seed, 'vertex', i, 'differs outside fog', p, q); continue
            # view z from Q (STQ.z = K q, q = 1 / z)
            z = K / q['stq'][2]
            f = max(min(max(a * z + b, fmin), fmax), 0)
            e = b + a2 * z
            want = max(min(f, e), 0)
            edged += want < f - 0.5
            tot += 1
            if abs(q['f'] - int(want)) > 1:
                bad += 1
                if bad < 6: print('seed', seed, 'vertex', i, 'z %.2f f %d want %.2f (plain %d)' % (z, q['f'], want, p['f']))
    print('edge fog: vertices %d (edge below the plain fog: %d), bad %d; cycles plain %d edge %d' % (tot, edged, bad, c0, c1))
    return bad

if __name__ == '__main__':
    sys.exit(1 if main() else 0)
