"""vu1map2.vsm (pipelined) vs vu1map.vsm on random R3 batches (incl. near / guard flagged and culled triangles)."""
import random, sys
from vuemu import VU1, vertices
import test_clip as tc

SRC = tc.SRC

def run(prog, entry, args, cull, clipstyle):
    vu = VU1(open(SRC + prog).read())
    tc.setup(vu, 'clip' if clipstyle else 'map', args, cull)
    for k in range(4):  # matrix .w row = z row (view .w = view z)
        q = vu.mem[k]; q[3] = q[2]
    cyc = vu.run(entry)
    return vertices(vu.kicks), cyc

def main(seeds=10, tris=54):
    bad = tot = adc = 0
    co = cn = 0
    for seed in range(seeds):
        rng = random.Random(100 + seed)
        args = tc.make(rng, tris, 0.3)
        cull = seed % 2
        vo, c1 = run('vu1map.vsm', 'VU1Map_CodeStart', args, cull, False)
        vn, c2 = run('vu1map2.vsm', 'VU1Map2_CodeStart', args, cull, True)
        co += c1; cn += c2
        assert len(vo) == len(vn), (len(vo), len(vn))
        for i, (a, b) in enumerate(zip(vo, vn)):
            if i % 3 == 2:
                adc += a['adc']
                if a['adc'] != b['adc']:
                    bad += 1; print('seed', seed, 'tri', i // 3, 'ADC', a['adc'], b['adc'])
                if a['adc']:
                    continue
            tot += 1
            d = max(abs(a['x'] - b['x']), abs(a['y'] - b['y']), abs(a['z'] - b['z']) >> 4, abs(a['f'] - b['f']),
                    max(abs(p - q) for p, q in zip(a['rgba'], b['rgba'])),
                    max(abs(p - q) / max(1e-9, abs(q)) * 1000 for p, q in zip(a['stq'], b['stq'])))
            if d > 2 and not (i % 3 != 2 and vo[i - i % 3 + 2]['adc']):
                bad += 1
                if bad < 5: print(seed, i, a, b)
    print('vertices compared %d, ADC tris %d, bad %d; cycles old %d new %d (%.2fx)' % (tot, adc, bad, co, cn, co / cn))

if __name__ == '__main__':
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 10)
