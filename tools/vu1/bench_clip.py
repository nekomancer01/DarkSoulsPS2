"""Cycles per triangle of the map clip program(s) on random batches (tools/vu1 timing model, vuemu):
python3 bench_clip.py [file.vsm entry ...]. Mixes: 'inside' = no corner flagged, 'some' = 30 % of the triangles near
the camera (test_clip.make near_frac 0.3), 'many' = 60 %."""
import random, sys
from vuemu import VU1
import test_clip as tc

MIXES = (('inside', 0.0), ('some', 0.3), ('many', 0.6))


def bench(path, entry, seeds=6, tris=32):
    res = {}
    for name, frac in MIXES:
        cyc = n = 0
        for seed in range(seeds):
            rng = random.Random(500 + seed)
            args = tc.make(rng, tris, frac)
            if frac == 0:  # push every triangle well inside the guard band and past the near plane
                org, step, K, vs = args
                vs = [([q[0] // 8, q[1] // 8, q[2] + 8192], st, col) for q, st, col in vs]
                args = org, step, K, vs
            vu = VU1(open(path).read())
            tc.setup(vu, 'clip', args, seed % 2)
            for k in range(4):
                q = vu.mem[k]; q[3] = q[2]
            cyc += vu.run(entry)
            n += tris
        res[name] = cyc / n
    return res


if __name__ == '__main__':
    a = sys.argv[1:] or [tc.SRC + 'vu1clip.vsm', 'VU1Clip_CodeStart']
    for path, entry in zip(a[::2], a[1::2]):
        r = bench(path, entry)
        print('%-40s %s' % (path.split('/')[-1], '  '.join('%s %.0f' % kv for kv in r.items())))
