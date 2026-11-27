#!/usr/bin/env python3
"""world.bin's masks written and read back (tools/ci/offline.sh; Polaris 4 finish).

tools/export_world.py pack_world writes the per-block block-local LNKL (load / keep) and RCHL (reach) runs and each
block's neighbour list (NBRS), src/world.c + src/wplan.c read them. This decodes a world.bin the way the C side does
(the Python decoder mirrors world.c world_init and wplan.c w_expand; it does not import them): every triangle's local
(load, keep, reach) masks, expanded through NBRS, are compared with the global masks the builders
(export_world.block_masks: worldlinks / worldreach.encode) gave.

  test_worldbin.py                       the synthetic fixture tools/ci/fixtures/world3.json through pack_world
  test_worldbin.py --world world.bin --gen <gen dir> --game <game dir> [--boot <map>]
                                         a real world.bin against block_masks recomputed from the game data
                                         (needs the exported collision.bin files under the gen dir)
Exit 0 = match, 1 = mismatch: `block <b> tris <first>..<end>: want <mask> got <mask>` per differing run."""
import argparse
import io
import json
import os
import struct
import sys
from contextlib import redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import chunkfile as chunk  # noqa: E402
import export_world  # noqa: E402
import worldreach  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "world3.json")


def _chunks(blob):
    f = chunk.File(blob, b"WRLD", "world.bin")
    return f, f.get(b"BLKS", 2)


def decode_blocks(blob):
    """[(pack, map, bounds (4), heights (2), flags, neighbour count)] from BLKS v2 (64 B per block)"""
    _, blks = _chunks(blob)
    out = []
    for o in range(0, len(blks), 64):
        p, m, *v = struct.unpack_from("<16s16s6f2I", blks, o)
        out.append((p.rstrip(b"\0").decode(), m.rstrip(b"\0").decode(), tuple(v[:4]), tuple(v[4:6]), v[6], v[7]))
    return out


def decode_local(blob, ntris=None):
    """NBRS + LRNL + LNKL + RCHL (v1, or v2 the wide layout) decoded and turned back to global masks through NBRS:
    (nbrs per block, [(block, first, triangles, load, keep)] per link, [(block, first, triangles, mask, distances)] per
    reach run, mask 0 / () for a run with none). A reach run covers its first triangle up to the next run's first; the
    last one up to ntris[block] (None: n = 0 there)"""
    f, blks = _chunks(blob)
    nb = len(blks) // 64
    v = f.chunks[b"NBRS"][0]
    assert v in (1, 2), f"world.bin: NBRS version {v}"
    nmax, lsz, esz = (15, 12, 8) if v == 1 else (31, 16, 12)
    nbrs_b, lrnl, lnkl, rchl = f.get(b"NBRS", v), f.get(b"LRNL", 1), f.get(b"LNKL", v), f.get(b"RCHL", v)
    assert len(nbrs_b) == (nmax + 1) * nb and len(lrnl) == 8 * nb, "world.bin: NBRS / LRNL size"
    nbrs = []
    for b in range(nb):
        c = nbrs_b[(nmax + 1) * b]
        ns = list(nbrs_b[(nmax + 1) * b + 1:(nmax + 1) * b + 1 + c])
        assert c <= nmax and ns == sorted(set(ns)) and b not in ns, f"block {b}: NBRS {ns} not ascending / has itself"
        assert c == struct.unpack_from("<I", blks, 64 * b + 60)[0], f"block {b}: BLKS neighbour count is not NBRS's"
        assert all(j < nb for j in ns), f"block {b}: NBRS {ns} names a block past the {nb} blocks"
        nbrs.append(ns)
    glob = lambda m, b: sum(1 << nbrs[b][k] for k in range(m.bit_length()) if m >> k & 1)
    links = []
    for b in range(nb):
        first, n = struct.unpack_from("<2I", lrnl, 8 * b)
        assert first + n <= len(lnkl) // lsz, f"block {b}: LNKL {first} + {n} past the end"
        for k in range(first, first + n):
            if v == 1:
                t0, nt, load, keep, _z = struct.unpack_from("<IHHHH", lnkl, lsz * k)
            else:
                t0, nt, _z, load, keep = struct.unpack_from("<IHHII", lnkl, lsz * k)
            assert load < 1 << len(nbrs[b]) and keep < 1 << len(nbrs[b]), f"block {b}: local bit past NBRS"
            links.append((b, t0, nt, glob(load, b), glob(keep, b)))
    nruns, nent = struct.unpack_from("<2I", rchl, 0)
    o = 12
    rr = [struct.unpack_from("<2I", rchl, o + 8 * b) for b in range(nb)]
    o += 8 * nb
    firsts = struct.unpack_from(f"<{nruns}H", rchl, o)
    ents = rchl[o + 2 * nruns:o + 3 * nruns]
    o += (3 * nruns + 3) & ~3
    assert o + esz * nent <= len(rchl), "world.bin: RCHL entries past the chunk"
    mfmt = "<H" if v == 1 else "<I"
    mw = 2 if v == 1 else 4
    tab = [(struct.unpack_from(mfmt, rchl, o + esz * k)[0], tuple(rchl[o + esz * k + mw:o + esz * k + mw + 6]))
           for k in range(nent)]
    reach = []
    for b, (r0, rn) in enumerate(rr):
        for k in range(r0, r0 + rn):
            end = firsts[k + 1] if k + 1 < r0 + rn else (ntris[b] if ntris else firsts[k])
            e = ents[k]
            mask, ds = tab[e - 1] if e else (0, ())
            assert mask < 1 << len(nbrs[b]), f"block {b}: local reach bit past NBRS"
            reach.append((b, firsts[k], end - firsts[k], glob(mask, b), ds[:bin(mask).count("1")]))
    return nbrs, links, reach


def decoded_links(ntris, rows):
    """per block per triangle (load, keep) from decode_local's link rows"""
    got = [[(0, 0)] * n for n in ntris]
    for b, t0, nt, load, keep in rows:
        for t in range(t0, t0 + nt):
            got[b][t] = (load, keep)
    return got


def decoded_reach(ntris, rows):
    """per block per triangle (mask, distances) from decode_local's reach rows"""
    got = [[(0, ())] * n for n in ntris]
    for b, t0, nt, mask, ds in rows:
        for t in range(t0, t0 + nt):
            got[b][t] = (mask, ds)
    return got


def resolve_reach(rc):
    """a block's reach per triangle as the runs give it: a wall (None) takes the reach of the run it lies in (the
    one before it, a leading wall the first one after)"""
    out, cur = [], None
    for r in rc:
        if r is not None:
            cur = r
        out.append(cur)
    first = next((r for r in rc if r is not None), (0, ()))
    return [r if r is not None else first for r in out]


def runs(want, got):
    """[(first, end, want, got)] of consecutive triangles that differ"""
    out, t = [], 0
    while t < len(want):
        if want[t] == got[t]:
            t += 1
            continue
        e = t
        while e < len(want) and want[e] == want[t] and got[e] == got[t]:
            e += 1
        out.append((t, e, want[t], got[t]))
        t = e
    return out


def compare(blob, names, per):
    """per: export_world.block_masks' (masks, reach) per block (global masks). Returns the mismatch lines"""
    ntris = [len(m) for m, _ in per]
    bad = []
    # the local chunks decoded through NBRS: equal to the builders' global masks on every triangle
    _, lrows, rrows = decode_local(blob, ntris)
    links = decoded_links(ntris, lrows)
    reach = decoded_reach(ntris, rrows)
    for b, (masks, rc) in enumerate(per):
        for t0, t1, w, g in runs([tuple(m) for m in masks], links[b]):
            bad.append(f"block {names[b]} tris {t0}..{t1}: want load/keep {w[0]:#x}/{w[1]:#x} got {g[0]:#x}/{g[1]:#x}")
        for t0, t1, w, g in runs(resolve_reach(rc), reach[b]):
            bad.append(f"block {names[b]} tris {t0}..{t1}: want reach {w[0]:#x} {list(w[1])} got {g[0]:#x} {list(g[1])}")
    return bad


def fixture():
    """(blocks, heights, nbr, per) of tools/ci/fixtures/world3.json as export_world.main builds them"""
    with open(FIXTURE) as f:
        d = json.load(f)
    blocks = [(b["name"], b["map"], tuple(b["bounds"]), [tuple(r) for r in b["rect"]] if b["rect"] else None)
              for b in d["blocks"]]
    heights = [tuple(b["heights"]) for b in d["blocks"]]
    per = []
    for masks, rc in zip(d["masks"], d["reach"]):
        assert len(masks) == len(rc)
        # export_world.block_masks: worldreach.encode(d) for walkable with reach, (0, ()) walkable none, None a wall
        per.append(([list(m) for m in masks],
                    [None if r is None else worldreach.encode({int(j): x for j, x in r.items()}) if r else (0, ())
                     for r in rc]))
    return blocks, heights, d["nbr"], per


def real(args):
    """a real world.bin against block_masks recomputed from the game data and the gen dir's collision.bin files"""
    import area
    blob = open(args.world, "rb").read()
    bl = decode_blocks(blob)
    names = [b[0] for b in bl]
    blocks = []
    for i, b in enumerate(names):
        m = area.BLOCKS[b]["map"] if b in area.BLOCKS else b
        col = os.path.join(args.gen, "collision.bin") if i == 0 else os.path.join(args.gen, b, "collision.bin")
        cells = area.BLOCKS.get(b, {}).get("cells")
        c = area.CELL
        rect = [(r[0] * c, r[1] * c, (r[2] + 1) * c, (r[3] + 1) * c) for r in area.cell_rects(cells)] if cells else None
        blocks.append((b, m, export_world.col_bounds(col), rect))
    nbr = export_world.block_nbr(args.game, blocks)  # (BLKS v2 holds only the count)
    per = [export_world.block_masks(args.game, names, nbr, blocks, i) for i in range(len(names))]
    return compare(blob, names, per)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--world")
    ap.add_argument("--gen")
    ap.add_argument("--game")
    args = ap.parse_args()
    if args.world:
        if not (args.gen and args.game):
            ap.error("--world needs --gen and --game")
        bad = real(args)
    else:
        blocks, heights, nbr, per = fixture()
        with redirect_stdout(io.StringIO()):  # pack_world's per-block summary
            blob = export_world.pack_world(blocks, heights, nbr, per)
        bad = compare(blob, [b[0] for b in blocks], per)
        # the table side too: names, bounds, neighbour lists (BLKS v2's count is NBRS's, decode_local asserts it)
        nbrs = decode_local(blob)[0]
        for i, (b, m, bb, hh, fl, nb) in enumerate(decode_blocks(blob)):
            if (b, m, bb, hh, fl, nb) != (blocks[i][0], blocks[i][1], blocks[i][2], heights[i], int(i == 0),
                                          len(nbrs[i])):
                bad.append(f"block {blocks[i][0]}: BLKS row differs: got {(b, m, bb, hh, fl, nb)}")
        for i, (masks, rc) in enumerate(per):
            if nbrs[i] != export_world.worldlinks.neighbours(i, nbr[i], masks, rc):
                bad.append(f"block {blocks[i][0]}: NBRS {nbrs[i]}")
        # only the block-local chunks (the global LRNG / LINK / REAC are gone; world.c refuses a file without NBRS)
        ids = set(chunk.File(blob, b"WRLD").chunks)
        if ids != {b"BLKS", b"NBRS", b"LRNL", b"LNKL", b"RCHL"}:
            bad.append(f"world.bin chunks {sorted(ids)}")
        # the wide layout (a block over NBR_MAX neighbours) read back the same way
        old = export_world.NBR_MAX
        export_world.NBR_MAX = max(len(x) for x in nbrs) - 1
        try:
            with redirect_stdout(io.StringIO()):
                wide = export_world.pack_world(blocks, heights, nbr, per)
        finally:
            export_world.NBR_MAX = old
        assert chunk.File(wide, b"WRLD").chunks[b"NBRS"][0] == 2
        bad += [f"wide: {x}" for x in compare(wide, [b[0] for b in blocks], per)]
        # the fixture has to exercise the paths (a run of zero masks, a wall inside a reach run, two reach entries)
        assert any(r is None for _, rc in per for r in rc) and len({r for _, rc in per for r in rc if r and r[0]}) > 2
    for line in bad:
        print(line)
    print(f"worldbin: {'FAIL, ' + str(len(bad)) + ' mismatches' if bad else 'ok'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
