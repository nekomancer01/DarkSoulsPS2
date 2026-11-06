"""vu1mapi.vsm (indexed map batches) vs vu1map2.vsm on the same triangles: random R3 batches (incl. near / guard
flagged and culled triangles, the edge fog loop) sent once as lists, once as shared vertices + triangle indices.
The kicked GIF packets must be identical (but for ST .w, which the GS ignores)."""
import os, random, sys
from vuemu import VU1, U, vertices
import gen
import test_clip as tc

SRC = tc.SRC


def index_export(vs):
    """the same through tools/export_map.py index_chunk (the bytes the exporter writes)"""
    import struct
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    import export_map
    n = len(vs)
    pos = b''.join(struct.pack('<3h', *q) for q, st, c in vs)
    st = b''.join(struct.pack('<2H', *s) for q, s, c in vs)
    col = bytes(x for q, s, c in vs for x in c)
    nv, p2, s2, c2, idx = export_map.index_chunk(n // 3, pos, st, col)
    verts = [(list(struct.unpack_from('<3h', p2, 6 * i)), list(struct.unpack_from('<2H', s2, 4 * i)),
              list(c2[4 * i:4 * i + 4])) for i in range(nv)]
    tris = [list(idx[4 * t:4 * t + 3]) for t in range(n // 3)]
    assert all(b == 0 for b in idx[3::4])
    return verts, tris


def index(vs, rng):
    """list corners -> (unique vertices padded to a multiple of 3, shuffled; triangles as vertex numbers)"""
    keys = list(dict.fromkeys((tuple(q), tuple(st), tuple(c)) for q, st, c in vs))
    rng.shuffle(keys)
    num = {k: i for i, k in enumerate(keys)}
    while len(keys) % 3:
        keys.append(keys[-1])
    tris = [[num[(tuple(q), tuple(st), tuple(c))] for q, st, c in vs[3 * t:3 * t + 3]] for t in range(len(vs) // 3)]
    return [(list(q), list(st), list(c)) for q, st, c in keys], tris


def run(prog, entry, args, cull, edge, idx=None):
    vu = VU1(open(SRC + prog).read())
    tc.setup(vu, 'clip', args, cull)  # header 0-6 and the list vertices at 7
    for k in range(4):
        q = vu.mem[k]; q[3] = q[2]
    top = gen.MAP_IN
    if edge:
        vu.mem[top][3] = U(-0.004) | 1
    if idx is not None:
        verts, tris = idx
        H = gen.MAPI_HDR
        start = H + 3 * len(verts)
        vu.wq(top + 7, [len(verts) // 3, start, 0, 0])
        for i, (q, st, col) in enumerate(verts):
            a = top + H + 3 * i
            vu.wq(a, [x & 0xFFFFFFFF for x in q] + [0])
            vu.wq(a + 1, st + [0, 0])
            vu.wq(a + 2, col)
        for t, (a, b, c) in enumerate(tris):
            vu.wq(top + start + t, [a, b, c, 0])
        assert start + len(tris) <= gen.MAP_IN_QW, (start, len(tris))
    cyc = vu.run(entry)
    return vu.kicks, cyc


def main(seeds=12, tris=54):
    # sharing is set per seed: some batches reach 3 vertices per triangle (vertex numbers past 85, i.e. 3 v > 255)
    bad = 0
    cl = ci = 0
    nv = nt = 0
    for seed in range(seeds):
        rng = random.Random(300 + seed)
        args = tc.make(rng, tris, 0.3)
        org, step, K, vs = args
        # share corners: each triangle reuses up to two corners of earlier ones (a mesh-like batch)
        vs = list(vs)
        for t in range(1, tris):
            for k in range(rng.randrange(3)):
                vs[3 * t + k] = vs[rng.randrange(3 * t)]
        args = (org, step, K, vs)
        idx = index_export(vs) if seed % 2 else index(vs, rng)  # odd seeds: the exporter's own bytes
        nv += len(idx[0]); nt += tris
        cull, edge = seed % 2, seed % 3 == 0
        kl, c1 = run('vu1map2.vsm', 'VU1Map2_CodeStart', args, cull, edge)
        ki, c2 = run('vu1mapi.vsm', 'VU1MapI_CodeStart', args, cull, edge, idx)
        cl += c1; ci += c2
        ki = [[(r, q[:3] + (0,), x) if r == 2 else (r, q, x) for r, q, x in k] if isinstance(k, list) else k
              for k in ki]  # packed ST ignores .w (the vertex's clip flag, pass 1)
        if kl != ki:
            bad += 1
            vl, vi = vertices(kl), vertices(ki)
            print('seed %d: %d / %d kicks differ' % (seed, sum(a != b for a, b in zip(kl, ki)), len(kl)))
            for i, (a, b) in enumerate(zip(vl, vi)):
                if a != b:
                    print('  vertex', i, a, b)
                    break
    print('%d batches, %.2f vertices / triangle, bad %d; cycles list %d indexed %d (%.2fx)' %
          (seeds, nv / nt, bad, cl, ci, cl / ci))
    return bad


def main_clip(seeds=12, tris=32):
    """vu1clip.vsm's indexed entry (VU1Clip_Idx) vs its list entry on the same near-plane / guard-band batches"""
    bad = 0
    out = 0
    for seed in range(seeds):
        rng = random.Random(500 + seed)
        args = tc.make(rng, tris, 0.5)
        org, step, K, vs = args
        vs = list(vs)
        for t in range(1, tris):
            for k in range(rng.randrange(3)):
                vs[3 * t + k] = vs[rng.randrange(3 * t)]
        args = (org, step, K, vs)
        idx = index_export(vs) if seed % 2 else index(vs, rng)
        assert gen.MAPI_HDR + 3 * len(idx[0]) + tris <= gen.CLIPI_MAX
        cull = seed % 2
        kl, _ = run('vu1clip.vsm', 'VU1Clip_CodeStart', args, cull, False)
        ki, _ = run('vu1clip.vsm', 'VU1Clip_Idx', args, cull, False, idx)
        out += len(kl)
        if kl != ki:
            bad += 1
            print('clip seed %d: %d / %d kicks differ (%d / %d)' % (seed, sum(a != b for a, b in zip(kl, ki)),
                                                                   len(kl), len(kl), len(ki)))
    print('clip: %d batches, %d kicks, bad %d' % (seeds, out, bad))
    return bad


if __name__ == '__main__':
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    sys.exit(1 if main(n) + main_clip(n) else 0)
