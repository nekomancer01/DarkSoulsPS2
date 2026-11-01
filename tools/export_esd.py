"""Pack a DSR EzState file (chr/<chr>.esd.dcx, default c0000) for the PS2 ESD runtime (ps2/proto/src/esd.c).
Usage: export_esd.py <game dir> <out esd.bin> [name: c0000 (default) or enemyCommon (shared by every enemy; the
mNN.chresdbnd files only hold a dummy.esd)]

Layout (little endian, all tables 4-byte aligned, offsets from the start of the blob):
  header  "ESD1", u32 groups, states, conds, cmds, args, idx, code_len,
          u32 offsets: groups, states, conds, cmds, args, idx, code                               (60 B)
  group   i32 id, u16 first_state, u16 states                                                     (8 B)
  state   i32 id, u16 entry, n_entry, exit, n_exit, while, n_while, conds (into idx), n_conds     (20 B)
  cond    i16 target (global state index, -1 = none), u16 pass, n_pass, subs (into idx), n_subs,
          u16 code_len, u32 code_off                                                               (16 B)
  cmd     i16 bank, i16 id, u16 args, u16 n_args                                                  (8 B)
  arg     u32 code_off, u16 code_len, u16 pad                                                     (8 B)
  idx     u16 condition indices (state condition lists and sub-condition lists)
  code    expression bytecode, unchanged from the ESD (see esd.c for the opcodes)"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
from esd import ESD


def main():
    game, out = sys.argv[1], sys.argv[2]
    chr_id = sys.argv[3] if len(sys.argv) > 3 else "c0000"
    e = ESD(souls.read(os.path.join(game, "chr", f"{chr_id}.esd.dcx")))
    b, ds = e.b, e.ds
    code = bytearray()
    code_at = {}

    def add_code(off, n):
        key = (off, n)
        if key not in code_at:
            code_at[key] = len(code)
            code.extend(b[ds + off:ds + off + n])
        return code_at[key], n

    groups, states, conds, cmds, args, idx = [], [], [], [], [], []
    state_index, cond_index = {}, {}

    # global state indices first (targets may point forward)
    order = []
    for gid in sorted(e.groups):
        gs = e.group_states(gid)
        # state 0 first: the group starts in its lowest state id
        offs = sorted(gs, key=lambda o: gs[o]["id"])
        groups.append((gid, len(order), len(offs)))
        for o in offs:
            state_index[o] = len(order)
            order.append(o)

    def raw_cmds(off, n):
        start = len(cmds)
        for k in range(n):
            bank, cid, a_off, a_n = struct.unpack_from("<iiqq", b, ds + off + 0x18 * k)
            a0 = len(args)
            for j in range(a_n):
                ao, al = struct.unpack_from("<qq", b, ds + a_off + 0x10 * j)
                args.append(add_code(ao, al))
            cmds.append((bank, cid, a0, a_n))
        return start, n

    def cond(coff):
        if coff in cond_index:
            return cond_index[coff]
        target, pc_off, pc_n, sc_off, sc_n, ev_off, ev_len = struct.unpack_from("<7q", b, ds + coff)
        ci = len(conds)
        cond_index[coff] = ci
        conds.append(None)
        p0, pn = raw_cmds(pc_off, pc_n)
        subs = [cond(s) for s in (struct.unpack_from(f"<{sc_n}q", b, ds + sc_off) if sc_n else [])]
        s0 = len(idx)
        idx.extend(subs)
        co, cl = add_code(ev_off, ev_len)
        conds[ci] = (state_index[target] if target != -1 else -1, p0, pn, s0, len(subs), cl, co)
        return ci

    for o in order:
        s = e.states[o]
        sid, c_off, c_n, en_off, en_n, ex_off, ex_n, wh_off, wh_n = struct.unpack_from("<9q", b, ds + o)
        en = raw_cmds(en_off, en_n)
        ex = raw_cmds(ex_off, ex_n)
        wh = raw_cmds(wh_off, wh_n)
        cl = [cond(c) for c in s["conds"]]
        c0 = len(idx)
        idx.extend(cl)
        states.append((sid, *en, *ex, *wh, c0, len(cl)))

    assert max(len(states), len(conds), len(cmds), len(args), len(idx)) < 65536
    sec = [struct.pack("<iHH", *g) for g in groups], [struct.pack("<i8H", *s) for s in states], \
        [struct.pack("<h5HI", *c) for c in conds], [struct.pack("<hhHH", *c) for c in cmds], \
        [struct.pack("<IHH", o, n, 0) for o, n in args], [struct.pack(f"<{len(idx)}H", *idx)], [bytes(code)]
    blob, offs, HDR = bytearray(), [], 60
    for parts in sec:
        while len(blob) % 4:
            blob.append(0)
        offs.append(HDR + len(blob))
        for p in parts:
            blob.extend(p)
    hdr = struct.pack("<4s7I7I", b"ESD1", len(groups), len(states), len(conds), len(cmds), len(args), len(idx),
                      len(code), *offs)
    with open(out, "wb") as f:
        f.write(hdr + blob)
    print(f"{len(groups)} groups, {len(states)} states, {len(conds)} conds, {len(cmds)} cmds, {len(args)} args, "
          f"{len(code)} B code -> {len(hdr) + len(blob)} B")


main()
