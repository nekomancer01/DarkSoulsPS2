#!/usr/bin/env python3
"""Per-block report and budget gate (Polaris 3.6): one report per world block, from the export's own outputs, and a
build step that fails on a budget miss.

  blockreport.py <out.json> <gen dir> [<gen dir> ...] [--scan build/scan/files.json] [--check world_budget.json] [--strict]

The gen dirs are the world's blocks as the Makefile's PVS_PEERS lists them: the boot block's gen dir ($(G)) first,
then $(G)/<block> for each WORLD_BLOCKS entry. MAP names the boot block's map (as for world_budget.py).

Per block (<out.json> "blocks", and blockreport/<block>.json next to it):
  levels    triangles and bytes per detail level: near (cells.bin), coarse / kept / backdrop / sky (map.bin resident
            chunks by flag), horizon (hzn.bin), far panorama (far.bin); bytes of near / coarse / kept / backdrop from
            map.bin.cells.json, the sidecar tools/world_budget.py reads
  residency the hard tier at the block's own navmesh spots (all blocks' items the arena holds there: near level within
            RES_NEAR_R, coarse and kept within RES_PIN_R) against the visual room, and the block's residency.h row
            (RES_BLOCK) against that room. The numbers are tools/world_budget.py's own: it is run here (its main(),
            unchanged), and this report reads its per-spot totals (world_budget.LAST)
  vram      per GS owner (src/gs_budget.h rows): FX = fx.bin textures (PSMT8 + CT32 CLUT, as export_effects.py counts
            them), OBJ = objects.bin texture models (animated + breakable sets; the 'DSRB' blob or the Polaris 3.3 chunk file), CHR = every enemy model.bin in the
            block (the runtime pages them: reported, not gated), WORLD = maptex.bin size (the textures stream through
            the map pixel pool: reported)
  scripts   EMEVD instruction classes (events.txt, export_emevd.py's line), Lua opcodes of the packed ai_map.lua0 (and
            the boot block's ai_common.lua0) against those lua0.c executes
  effects   FFX action data subtypes of the block's effect bundle: ported / not (from the coverage scan's per-file
            results, tools/scan/scan.py; "no scan" without them)

--check <world_budget.json>: the world_budget.py output the build wrote ($(G)/world_budget.json) must equal what this
run of world_budget.py computed (blocks, room, tiers, contract); exit 1 when it does not (stale inputs).

--strict exits 1 on a miss: world_budget.py's own --strict verdict (hard tier over the room or on the seam
allowance past RES_ALLOW_PCT / RES_ALLOW_BLOCK_PCT, a RES_BLOCK row over the visual room), a block whose FX or OBJ
textures exceed their gs_budget.h row (the runtime refuses those uploads past the row, gs.c gs_vram_admit), a set of
blocks on together at a seam spot (world.bin links, spot_live_vram) whose FX / OBJ / CHR textures add up past the row
("spot a+b fx live n KB > row r KB"; an upper bound: whole per-block sets, CHR = every enemy type of the block), a packed
Lua opcode lua0.c does not execute. EMEVD instructions export_emevd.py does not support are reported (Depths and
Darkroot blocks have some today: TECH_DEBT scan-2), not gated.
BLOCKREPORT_STRICT=0 in the environment turns --strict into a report (the registered fallback switch)."""
import contextlib
import io
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs_budget  # noqa: E402
import mapcfg  # noqa: E402
import residency  # noqa: E402

CELL_PART = 0x80000000  # cells.bin: a later part of the cell before it (mapshot.py)
# owners the runtime refuses past their row (gs_budget.h) whose sets here are what is live at once. CHR is refused too
# but reported only: its figure sums every enemy type's model.bin textures of a block, which the runtime pages in by
# distance (enemy.c), so the sum is no live set (boot block 277 KB, Parish 224 KB against the 176 KB row, 2026-11-28)
GATED_VRAM = ("fx", "obj")


def _u32(b, o, n=1):
    v = struct.unpack_from(f"<{n}I", b, o)
    return v if n > 1 else v[0]


def _chunk_tris(b, off, n):
    """n chunk records (64 B, map.c Chunk) at off -> [(triangles, flags)]"""
    out = []
    for i in range(n):
        _, fl, nw = struct.unpack_from("<iII", b, off + 64 * i)
        out.append((nw & 0xFFFF, fl))
    return out


def levels(gen):
    """{level: dict(tris, chunks, kb)} of one block's map export"""
    lv = {k: dict(tris=0, chunks=0, kb=0.0) for k in ("near", "coarse", "kept", "backdrop", "sky", "horizon", "far")}
    p = os.path.join(gen, "map.bin")
    if not os.path.exists(p):
        return None
    b = open(p, "rb").read()
    magic, nch = b[:4], _u32(b, 4)
    och = _u32(b, 36)
    for t, fl in _chunk_tris(b, och, nch):
        k = "sky" if fl & mapcfg.CHUNK_SKY else "backdrop" if fl & mapcfg.CHUNK_BACKDROP else \
            "coarse" if fl & mapcfg.CHUNK_COARSE else "kept"
        lv[k]["tris"] += t
        lv[k]["chunks"] += 1
    ocell = _u32(b, 76) if magic == b"DSM5" else 0
    cp = os.path.join(gen, "cells.bin")
    if ocell and os.path.exists(cp):
        cb = open(cp, "rb").read()
        for i in range(_u32(b, ocell)):
            off, size, nn = _u32(b, ocell + 16 + 48 * i + 24, 3)
            if size:
                for t, _ in _chunk_tris(cb, off, nn & ~CELL_PART):
                    lv["near"]["tris"] += t
                    lv["near"]["chunks"] += 1
    for name, key, magic_, hdr in (("hzn.bin", "horizon", b"HZN1", "hzn"), ("far.bin", "far", b"FARP", "far")):
        fp = os.path.join(gen, name)
        if not os.path.exists(fp):
            continue
        fb = open(fp, "rb").read()
        lv[key]["kb"] = round(len(fb) / 1024, 1)
        if fb[:4] != magic_:
            continue
        if hdr == "hzn":
            nch_, ncell = _u32(fb, 4, 2)
            recs = _chunk_tris(fb, 16 + 32 * ncell, nch_)
        else:
            ns, nc = _u32(fb, 4, 2)
            recs = _chunk_tris(fb, 16 + 32 * ns, nc)
        lv[key]["tris"] = sum(t for t, _ in recs)
        lv[key]["chunks"] = len(recs)
    jp = os.path.join(gen, "map.bin.cells.json")
    if os.path.exists(jp):
        j = json.load(open(jp))
        cols = [0.0] * 4
        for c in j["cells"]:
            for k in range(4):
                cols[k] += c[9 + k]
        for k, v in zip(("near", "coarse", "kept", "backdrop"), cols):
            lv[k]["kb"] = round(v / 1024, 1)
        lv["sky"]["kb"] = round(j.get("sky", 0) / 1024, 1)
    return lv


def _dsrm_tex(b, o):
    """VRAM bytes of a DSRM model's textures (export_model.py: u32 w, h, clut, pixels; PSMT8 + 256-entry CT32 CLUT)"""
    if b[o:o + 4] != b"DSRM":
        return 0, 0
    ntex, otex = _u32(b, o + 20), _u32(b, o + 48)
    tot = 0
    for i in range(ntex):
        w, h = _u32(b, o + otex + 16 * i, 2)
        tot += w * h + 1024
    return tot, ntex


def chunks(b):
    """a chunk file (Polaris 3.3, tools/chunkfile.py: 'CHNK', file id, total, count; chunks u32 id, u16 version, u16 0,
    u32 payload bytes, u32 0, payload padded to 16) -> (file id, {chunk id: payload offset in b, payload bytes})"""
    n, pos, out = _u32(b, 12), 16, {}
    for _ in range(n):
        cid, size = b[pos:pos + 4], _u32(b, pos + 8)
        out[cid] = (pos + 16, size)
        pos += 16 + size + (-size % 16)
    return b[4:8], out


def _objs_tex(b):
    """VRAM bytes and texture count of a chunked objects.bin (file id OBJS): the ATEX / BTEX texture models, and each
    breakable type's own model (TYPE -> MODL) when there is no BTEX"""
    _, c = chunks(b)
    tot = ntex = 0
    for cid in (b"ATEX", b"BTEX"):
        if cid in c:
            t, n = _dsrm_tex(b, c[cid][0])
            tot, ntex = tot + t, ntex + n
    if b"BTEX" not in c and b"TYPE" in c and b"MODL" in c:
        to, tsz = c[b"TYPE"]
        for i in range(tsz // 64):
            t, n = _dsrm_tex(b, c[b"MODL"][0] + _u32(b, to + 64 * i + 4))
            tot, ntex = tot + t, ntex + n
    return tot, ntex


def vram(gen):
    """{owner: dict(kb, textures, row_kb, gated)}"""
    out = {}
    fp = os.path.join(gen, "fx.bin")
    if os.path.exists(fp):
        b = open(fp, "rb").read()
        if b[:4] == b"DSFX":
            ntex, otex = _u32(b, 8), _u32(b, 32)
            tot = sum(w * h + 1024 for w, h in (struct.unpack_from("<HH", b, otex + 16 * i) for i in range(ntex)))
            out["fx"] = dict(kb=round(tot / 1024, 1), textures=ntex)
        else:  # a format this report does not read (a chunk-stream fx.bin, Polaris 3.3): a miss, never a silent 0
            out["fx"] = dict(kb=None, unread=b[:4].decode("latin1"))
    op = os.path.join(gen, "objects.bin")
    if os.path.exists(op):
        b = open(op, "rb").read()
        if b[:4] == b"DSRB":
            ver = _u32(b, 4)
            tot = ntex = 0
            offs = []
            if ver >= 3:
                offs.append(_u32(b, 44))  # animated types' shared texture model
            if ver >= 4:
                offs.append(_u32(b, 56))  # breakables' shared texture model (the whole MSB's set)
            for o in offs:
                if o:
                    t, n = _dsrm_tex(b, o)
                    tot, ntex = tot + t, ntex + n
            if ver < 4 or not (len(offs) > 1 and offs[1]):  # breakable types carry their own textures
                types, toff = _u32(b, 8), _u32(b, 16)
                for i in range(types):
                    mo = _u32(b, toff + 64 * i + 4)
                    t, n = _dsrm_tex(b, mo)
                    tot, ntex = tot + t, ntex + n
            out["obj"] = dict(kb=round(tot / 1024, 1), textures=ntex)
        elif b[:4] == b"CHNK" and b[4:8] == b"OBJS":
            tot, ntex = _objs_tex(b)
            out["obj"] = dict(kb=round(tot / 1024, 1), textures=ntex)
        else:
            out["obj"] = dict(kb=None, unread=b[:4].decode("latin1"))
    chr_kb = chr_n = 0
    for d in sorted(os.listdir(gen)):
        mp = os.path.join(gen, d, "model.bin")
        if re.fullmatch(r"c\d{4}", d) and os.path.exists(mp):
            t, n = _dsrm_tex(open(mp, "rb").read(), 0)
            chr_kb, chr_n = chr_kb + t, chr_n + n
    out["chr"] = dict(kb=round(chr_kb / 1024, 1), textures=chr_n, types=sum(
        1 for d in os.listdir(gen) if re.fullmatch(r"c\d{4}", d)))
    mt = os.path.join(gen, "maptex.bin")
    if os.path.exists(mt):
        out["world"] = dict(kb=round(os.path.getsize(mt) / 1024, 1), note="maptex.bin bytes (streamed through the pool)")
    for k, v in out.items():
        v["row_kb"] = gs_budget.GSB.get(k)
        v["gated"] = k in GATED_VRAM
        v["over"] = bool(v["gated"] and v["row_kb"] is not None and v["kb"] is not None and v["kb"] > v["row_kb"])
    return out


def spot_on_sets(boot):
    """world.bin of the boot gen dir -> (block names, [on set of every link as a sorted tuple of block indices]) or None.
    The same masks tools/world_budget.py structural() reads (LNKL: per link the blocks it loads and keeps, as local bits
    of the block's NBRS list): a link's on set = load + keep + the block itself, the blocks resident at that spot"""
    p = os.path.join(boot, "world.bin")
    if not os.path.exists(p):
        return None
    import chunkfile as chunk
    try:
        w = chunk.read(p, b"WRLD")
        bl, lr, lk, ns = w.get(b"BLKS", 2), w.get(b"LRNL", 1), w.get(b"LNKL", 1), w.get(b"NBRS", 1)
    except chunk.ChunkError:
        return None
    n = len(bl) // 64
    names = [bl[64 * i:16 + 64 * i].split(b"\0")[0].decode() for i in range(n)]
    lr = struct.unpack_from(f"<{2 * n}I", lr, 0)
    nbrs = [list(ns[16 * i + 1:16 * i + 1 + ns[16 * i]]) for i in range(n)]
    sets = set()
    for blk in range(n):
        for l in range(lr[2 * blk + 1]):
            _, _, load, keep, _ = struct.unpack_from("<IHHHH", lk, 12 * (lr[2 * blk] + l))
            on = {blk} | {nbrs[blk][k] for k in range(len(nbrs[blk])) if (load | keep) >> k & 1}
            sets.add(tuple(sorted(on)))
    return names, sorted(sets)


def spot_live_vram(boot, per_block):
    """the sets of blocks that are on together at a seam spot (world.bin links) against the gs_budget.h rows.
    per_block: {block name: vram() result}. Returns [(blocks, owner, live_kb, row_kb)], one per on set and gated owner
    whose sum is over its row, over two or more blocks (a single block is vram()'s own row check). The sum is an upper
    bound: it counts every on block's whole set, not only what is drawn at that spot. [] without world.bin"""
    r = spot_on_sets(boot)
    if r is None:
        return []
    names, sets = r
    out = []
    for on in sets:
        if len(on) < 2:
            continue
        for owner in GATED_VRAM:
            row = gs_budget.GSB.get(owner)
            kbs = [per_block.get(names[i] if i else os.path.basename(os.path.normpath(boot)), {}).get(owner, {})
                   .get("kb") for i in on]  # block 0 is the boot gen dir, whatever world.bin calls it
            if row is None or any(k is None for k in kbs):
                continue
            tot = round(sum(kbs), 1)
            if tot > row:
                out.append(([names[i] for i in on], owner, tot, row))
    return out


def lua_ops_in_pack(p):
    """opcode name -> count over every proto of a lua0 pack (lua50.py --pack: code unchanged Lua 5.0 encoding)"""
    import lua50
    b = open(p, "rb").read()
    if b[:4] != b"DSRL":
        return None
    counts = {}

    def proto(o):
        _, _, ncode, nk, npr = struct.unpack_from("<5I", b, o)
        o += 20
        for i in struct.unpack_from(f"<{ncode}I", b, o):
            op = i & 0x3F
            name = lua50.OPS[op] if op < len(lua50.OPS) else f"op{op}"
            counts[name] = counts.get(name, 0) + 1
        o += 4 * ncode
        for _ in range(nk):
            t = _u32(b, o)
            o += 4
            if t == 2:
                o += 4
            elif t == 3:
                n = _u32(b, o)
                o += 4 + (n + 3) // 4 * 4
        for _ in range(npr):
            o = proto(o)
        return o

    o = 8
    for _ in range(_u32(b, 4)):
        n = _u32(b, o)
        o += 4 + (n + 3) // 4 * 4
        o = proto(o)
    return counts


def scripts(gen, boot_gen):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "scan"))
    import support
    out = {}
    ep = os.path.join(gen, "events.txt")
    if os.path.exists(ep):
        for line in open(ep):
            m = re.match(r"(\S+): (\d+) events, (\d+) instrs: real (\d+) .*noop (\d+), stub (\d+), unsupported (\d+)",
                         line)
            if m:
                out["emevd " + m[1]] = dict(events=int(m[2]), instrs=int(m[3]), real=int(m[4]), noop=int(m[5]),
                                            stub=int(m[6]), unsupported=int(m[7]))
    ops = support.lua_ops()
    for name in ("ai_map.lua0", "ai_common.lua0"):
        p = os.path.join(gen if name == "ai_map.lua0" else boot_gen, name)
        if os.path.exists(p):
            c = lua_ops_in_pack(p)
            if c is not None:
                missing = {k: v for k, v in c.items() if k not in ops}
                out["lua " + name] = dict(instrs=sum(c.values()), outside_vm=missing)
    return out


def bundle_of(map_id):
    return f"sfx/FRPG_SfxBnd_{map_id[:6]}.ffxbnd.dcx"


def effects(map_id, scan):
    if scan is None:
        return dict(note="no scan")
    rows = scan.get(bundle_of(map_id))
    if rows is None:
        return dict(note="bundle not in the scan")
    ported = unported = 0
    missing = {}
    for kind, value, status, n, *_ in rows:
        if kind == "ffx action data":
            if status == "ok":
                ported += n
            else:
                unported += n
                missing[value] = missing.get(value, 0) + n
    tot = ported + unported
    return dict(action_data=tot, ported=ported, ported_pct=round(100.0 * ported / tot, 1) if tot else None,
                unported_by_subtype=missing)


def set_aside(paths):
    """a failed gate leaves no target make would take as up to date: each file it wrote becomes <file>.failed (kept
    to read), and an older copy under the real name goes too"""
    for p in paths:
        real = p[:-4] if p.endswith(".tmp") else p
        if os.path.exists(p):
            os.replace(p, real + ".failed")
        if os.path.exists(real):
            os.remove(real)


def main(argv):
    """the outputs appear under their names only when the gate passes (else <file>.failed): a rerun of make after a
    miss runs the gate again instead of finding a fresh blockreport.json"""
    written = []
    try:
        rc = report(argv, written)
    except BaseException:
        set_aside(written)
        raise
    if rc:
        set_aside(written)
    else:
        for p in written:
            if p.endswith(".tmp"):
                os.replace(p, p[:-4])
    return rc


def report(argv, written):
    strict = "--strict" in argv and os.environ.get("BLOCKREPORT_STRICT", "1") != "0"
    args = [a for a in argv if a != "--strict"]
    scan = None
    if "--scan" in args:
        i = args.index("--scan")
        sp = args[i + 1]
        del args[i:i + 2]
        if os.path.exists(sp):
            scan = json.load(open(sp))
    check = None
    if "--check" in args:
        i = args.index("--check")
        check = args[i + 1]
        del args[i:i + 2]
    out, dirs = args[0], args[1:]
    boot = dirs[0]
    import numpy as np
    import world_budget as wb
    # world_budget reads world.bin from its output's dir: point it at the boot block's gen dir
    real_structural = wb.structural
    wb.structural = lambda _d: real_structural(boot)
    log = io.StringIO()
    wb_out = os.path.splitext(out)[0] + ".world_budget.json.tmp"
    written.append(wb_out)
    with contextlib.redirect_stdout(log):
        wb_rc = wb.main([wb_out] + dirs + ["--strict"])
    wb.structural = real_structural
    res = json.load(open(wb_out))
    L = wb.LAST
    names = L.get("names", [])
    room = res["room_kb"]
    blocks, misses = [], []
    for d in dirs:
        name = os.path.basename(os.path.normpath(d))
        map_id = os.environ.get("MAP", "m10_02_00_00") if d == boot else name[:12]
        k = names.index(name) if name in names else None
        r = dict(block=name, map=map_id)
        r["levels"] = levels(d)
        wrow = next((b for b in res["blocks"] if b["block"] == name), None)
        rr = dict(world_budget=wrow, room_kb=room, contract=res["contract"]["blocks"].get(name))
        if k is not None:
            own = L["own"] == k
            hard = L["tot"]["hard"][own]
            gated = bool(L["gated"][own].any())
            rr.update(spots=int(own.sum()), gated=gated,
                      hard_kb=dict(p50=round(float(np.percentile(hard, 50))), p90=round(float(np.percentile(hard, 90))),
                                   max=round(float(hard.max()))) if len(hard) else None,
                      hard_over_spots=int((hard > room).sum()),
                      allowance_pct={kk: v for kk, v in res["hard"]["allowance_pct"].items() if kk.startswith(name + " ")})
        if rr["contract"] and rr["contract"]["room_kb"] > room:
            misses.append(f"{name}: residency.h room {rr['contract']['room_kb']} KB over the visual room {room} KB")
        r["residency"] = rr
        r["vram"] = vram(d)
        for owner, v in r["vram"].items():
            if v["over"]:
                misses.append(f"{name}: VRAM {owner} {v['kb']} KB over its gs_budget.h row {v['row_kb']} KB")
            if v.get("unread"):
                misses.append(f"{name}: VRAM {owner}: file format '{v['unread']}' not read by blockreport.py (update it)")
        r["scripts"] = scripts(d, boot)
        for key, v in r["scripts"].items():
            if key.startswith("lua") and v["outside_vm"]:
                misses.append(f"{name}: {key} uses opcodes lua0.c lacks: {v['outside_vm']}")
        r["effects"] = effects(map_id, scan)
        blocks.append(r)
        os.makedirs(os.path.join(os.path.dirname(os.path.abspath(out)), "blockreport"), exist_ok=True)
        bp = os.path.join(os.path.dirname(os.path.abspath(out)), "blockreport", name + ".json.tmp")
        written.append(bp)
        with open(bp, "w") as f:
            json.dump(r, f, indent=1)
    # the seam spots report only: whole per-block sets summed count a texture shared by several blocks' sets (fx.c vtex
    # reference counts, the shared ATEX object textures) once per block. On the 26-block world 2026-11-28 this listed 25
    # spots at 368-808 KB while the game logged one 1 KB FX refusal; gate it once the sets carry texture ids to dedupe
    spot_notes = [f"spot {'+'.join(on)} {owner} live {tot} KB > row {row} KB (upper bound)"
                  for on, owner, tot, row in spot_live_vram(boot, {r["block"]: r["vram"] for r in blocks})]
    if wb_rc:
        misses.insert(0, "world_budget.py --strict: " + next((ln for ln in log.getvalue().splitlines()
                                                            if "FAIL" in ln), "failed"))
    summary = dict(room_kb=room, world_budget_rc=wb_rc, gsb=gs_budget.GSB, misses=misses, spot_notes=spot_notes, blocks=blocks)
    written.append(out + ".tmp")
    with open(out + ".tmp", "w") as f:
        json.dump(summary, f, indent=1)
    for r in blocks:
        lv, rr, vr = r["levels"] or {}, r["residency"], r["vram"]
        tri = " ".join(f"{k} {v['tris']}" for k, v in lv.items() if v["tris"])
        hk = rr.get("hard_kb") or {}
        print(f"block report: {r['block']}: tris {tri}; hard tier p90 {hk.get('p90')} / max {hk.get('max')} KB of "
              f"{room} KB ({rr.get('hard_over_spots', 0)} spots over{', gated' if rr.get('gated') else ''}); VRAM "
              + ", ".join(f"{k} {v['kb']}" + (f"/{v['row_kb']}" if v.get("row_kb") else "") for k, v in vr.items())
              + " KB")
    print(f"block report: {len(blocks)} blocks, {len(misses)} budget misses -> {out}")
    for m in misses:
        print(f"block report: MISS {m}")
    for m in spot_notes:
        print(f"block report: note {m}")
    if check:
        ref = json.load(open(check))
        diff = sorted(k for k in set(ref) | set(res) if ref.get(k) != res.get(k))
        print(f"block report: {check} " + ("matches world_budget.py" if not diff else f"DIFFERS in {diff}"))
        if diff:
            return 1
    if misses and strict:
        print("block report: FAIL (--strict; BLOCKREPORT_STRICT=0 reports only)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
