"""Chain-level timing model of the map pass's texture uploads (texstream-4, vu1.c VU1_TEX_PATH3): how long VU1 stands
still for a frame's uploads with FLUSH + PATH2 in the chain (default build) and with PATH3 behind one FLUSHA gate.

  python3 path3_model.py [--batches 400] [--gate 0.0,0.25,0.5] [--self-test]

PCSX2 times neither VU1 nor the GIF, so this is a model. The VU1 side comes from the emulator: cycles of one full
vu1map2.vsm batch (54 triangles, random R3 input, tools/vu1/test_clip.py) via vuemu. The rest are ASSUMED rates:
  IMAGE_CYC  VU1 cycles per IMAGE qword over the GIF (1 qw / 2 bus cycles = 4 VU1 cycles; GS host -> local
             transfers run below the GIF's 1 qw / bus cycle peak)
  UNPACK_CYC VU1 cycles per qword VIF1 writes into VU memory (1 qw / bus cycle = 2 VU1 cycles)
Default build, per upload: FLUSH waits for the running batch, so the next batch's unpack no longer overlaps a running
program (one batch unpack lost), then the PATH2 header + IMAGE + TEXFLUSH run with VU1 idle.
PATH3 build, per segment: the queue runs from the kick, beside the batches in front of the gate (the share --gate of
the pass); at the gate VU1 waits for what is left of it, plus one lost unpack as above. PATH3 also takes GIF cycles from
PATH1 before the gate: counted as the IMAGE time shared, i.e. no free overlap beyond what the GIF has spare (the map
program kicks ~10 qw per triangle against ~40 VU1 cycles, so the GIF is idle most of the time; ASSUMED).
Upload sets: the TECH_DEBT texstream-4 numbers (first Firelink frame 216-264 KB, moving 2-32 KB per frame), split
into textures of the sizes map.bin has (128x128 T8 16 KB, 64x64 T8 4 KB, CLUTs 1 KB)."""
import argparse, random, sys
from vuemu import VU1
import test_clip as tc

IMAGE_CYC = 4
UNPACK_CYC = 2
HEADER_QW = 8  # PATH2: DMA tags + GIF tag + BITBLTBUF..TRXDIR + IMAGE tag + TEXFLUSH packet


def batch_cycles(tris=54, seeds=4):
    tot = 0
    for seed in range(seeds):
        vu = VU1(open(tc.SRC + 'vu1map2.vsm').read())
        tc.setup(vu, 'clip', tc.make(random.Random(100 + seed), tris, 0.0), seed % 2)
        for k in range(4):
            q = vu.mem[k]; q[3] = q[2]
        tot += vu.run('VU1Map2_CodeStart')
    return tot / seeds


def unpack_qw(tris=54):
    n = 3 * tris
    return 7 + 3 * n  # header + pos / st / colour, one qword per vertex each in VU memory


def cost_path2(uploads, unpack):
    """VU1 idle cycles: per upload one lost unpack + the transfer on PATH2"""
    return sum(unpack * UNPACK_CYC + (HEADER_QW + (b + 15) // 16) * IMAGE_CYC for b in uploads)


def cost_path3(uploads, unpack, before_gate):
    """VU1 idle cycles: one gate per segment; the queue overlaps the before_gate VU1 cycles in front of it"""
    if not uploads:
        return 0
    q = sum((8 + (b + 15) // 16) * IMAGE_CYC for b in uploads) + 2 * IMAGE_CYC
    return unpack * UNPACK_CYC + max(0, q - before_gate)


def upload_sets():
    rng = random.Random(7)
    first = [16384] * 8 + [4096] * 20 + [1024] * 67  # ~270 KB: mip 0 / mip 1 levels, CLUTs
    sets = {'first frame (~%d KB)' % (sum(first) // 1024): first}
    for kb in (2, 8, 32):
        s, left = [], kb * 1024
        while left > 0:
            b = min(left, rng.choice((1024, 4096, 16384)))
            s.append(b); left -= b
        sets['moving, %d KB' % kb] = s
    return sets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--batches', type=int, default=400, help='map batches per frame (EE log "batches")')
    ap.add_argument('--gate', default='0,0.1,0.5', help='share of the pass in front of the first upload')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    bc = batch_cycles()
    up = unpack_qw()
    us = 1e6 / 294.912e6
    print('vu1map2 54-tri batch: %.0f VU1 cycles (vuemu); unpack %d qw; map pass %d batches = %.2f ms' %
          (bc, up, a.batches, a.batches * bc * us / 1000))
    gates = [float(g) for g in a.gate.split(',')]
    print('%-22s %8s %10s ' % ('uploads', 'count', 'PATH2 us') + ' '.join('%12s' % ('PATH3 g=%.2f' % g) for g in gates))
    bad = 0
    for name, s in upload_sets().items():
        c2 = cost_path2(s, up)
        c3 = [cost_path3(s, up, g * a.batches * bc) for g in gates]
        bad += any(c > c2 + 2 * IMAGE_CYC for c in c3)
        print('%-22s %8d %10.1f ' % (name, len(s), c2 * us) + ' '.join('%12.1f' % (c * us) for c in c3))
    if a.self_test:
        rng = random.Random(1)
        for _ in range(2000):  # never worse than PATH2 by more than the closing TEXFLUSH packet (one upload, gate 0)
            s = [rng.choice((256, 1024, 4096, 16384, 65536)) for _ in range(rng.randint(1, 50))]
            bad += cost_path3(s, up, rng.random() * a.batches * bc) > cost_path2(s, up) + 2 * IMAGE_CYC
        print('self-test: %d cases worse than PATH2' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
