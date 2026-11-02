"""Lua 5.0 bytecode reader / disassembler for DSR's script/*.luabnd (compiled chunks: "\\x1bLuaP", little-endian,
int 4, size_t 8, Instruction 4, lua_Number double). Layout from Lua 5.0 lundump.c / lopcodes.h.

Usage: lua50.py <luabnd.dcx> [name filter]      disassemble matching files
       lua50.py <luabnd.dcx> --calls             globals / methods each file uses
       lua50.py --pack [--msb <game> <map>] <out.bin> <luabnd.dcx>[:file,file,...] ...
                                                 lua0 pack for ps2/proto/src/lua0.c (chunks run in this order;
                                                 --msb keeps only the battle / logic goals that map's enemies use)

lua0 pack (little-endian, 4-byte aligned): "DSRL", u32 chunks; per chunk u32 name length, name (padded to 4), proto.
proto: u32 nups | nparams << 8 | maxstack << 16, u32 line, ncode, nk, np, u32 code[ncode] (Lua 5.0 encoding,
unchanged), constants {u32 type (0 nil, 2 number, 3 string = lua0 LT_*); number: f32; string: u32 len, bytes padded
to 4}, then np sub-protos. lua_Number becomes float (the EE has no double FPU); debug info is dropped."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls

OPS = ["MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL", "SETUPVAL",
       "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "POW", "UNM", "NOT", "CONCAT", "JMP", "EQ", "LT",
       "LE", "TEST", "CALL", "TAILCALL", "RETURN", "FORLOOP", "TFORLOOP", "TFORPREP", "SETLIST", "SETLISTO", "CLOSE",
       "CLOSURE"]
MAXSTACK = 250  # RK operands >= this are constants


class Proto:
    pass


class _R:
    def __init__(self, b, p):
        self.b, self.p = b, p

    def u8(self):
        self.p += 1
        return self.b[self.p - 1]

    def i32(self):
        self.p += 4
        return struct.unpack_from("<i", self.b, self.p - 4)[0]

    def size(self):
        self.p += 8
        return struct.unpack_from("<Q", self.b, self.p - 8)[0]

    def num(self):
        self.p += 8
        return struct.unpack_from("<d", self.b, self.p - 8)[0]

    def str(self):
        n = self.size()
        if not n:
            return None
        s = self.b[self.p:self.p + n - 1]
        self.p += n
        return s.decode("latin1")


def load(b):
    if b[:5] != b"\x1bLuaP":
        raise ValueError("not a Lua 5.0 chunk")
    if tuple(b[5:14]) != (1, 4, 8, 4, 6, 8, 9, 9, 8):
        raise ValueError(f"unexpected header {tuple(b[5:14])}")
    r = _R(b, 14)
    r.num()  # test number
    return _proto(r)


def _proto(r):
    f = Proto()
    f.source = r.str()
    f.line = r.i32()
    f.nups, f.nparams, f.vararg, f.maxstack = r.u8(), r.u8(), r.u8(), r.u8()
    f.lines = [r.i32() for _ in range(r.i32())]
    f.locals = [(r.str(), r.i32(), r.i32()) for _ in range(r.i32())]
    f.upvals = [r.str() for _ in range(r.i32())]
    f.k = []
    for _ in range(r.i32()):
        t = r.u8()
        f.k.append(r.num() if t == 3 else r.str() if t == 4 else None)
    f.p = [_proto(r) for _ in range(r.i32())]
    f.code = [struct.unpack_from("<I", r.b, r.p + 4 * i)[0] for i in range(r.i32())]
    r.p += 4 * len(f.code)
    return f


def decode(i):
    op = i & 0x3F
    c = (i >> 6) & 0x1FF
    b = (i >> 15) & 0x1FF
    a = i >> 24
    bx = (i >> 6) & 0x3FFFF
    return op, a, b, c, bx, bx - 131071


def rk(f, x):
    return repr(f.k[x - MAXSTACK]) if x >= MAXSTACK else f"r{x}"


def disasm(f, name="main", depth=0, out=None):
    out = out if out is not None else []
    pad = "  " * depth
    out.append(f"{pad}function {name} (line {f.line}, {f.nparams} params{', ...' if f.vararg else ''}, "
               f"{f.nups} upvalues, {len(f.code)} instructions)")
    for pc, ins in enumerate(f.code):
        op, a, b, c, bx, sbx = decode(ins)
        n = OPS[op] if op < len(OPS) else f"OP{op}"
        if n in ("LOADK", "GETGLOBAL", "SETGLOBAL"):
            arg = f"r{a} {f.k[bx]!r}"
        elif n in ("JMP", "FORLOOP", "TFORPREP"):
            arg = f"r{a} -> {pc + 1 + sbx}"
        elif n == "CLOSURE":
            arg = f"r{a} proto {bx}"
        elif n in ("GETTABLE", "SELF"):
            arg = f"r{a} r{b} [{rk(f, c)}]"
        elif n == "SETTABLE":
            arg = f"r{a}[{rk(f, b)}] = {rk(f, c)}"
        elif n in ("ADD", "SUB", "MUL", "DIV", "POW", "EQ", "LT", "LE"):
            arg = f"r{a} {rk(f, b)} {rk(f, c)}"
        else:
            arg = f"r{a} {b} {c}"
        ln = f.lines[pc] if pc < len(f.lines) else ""
        out.append(f"{pad}  {pc:4d} [{ln}] {n:9s} {arg}")
    for k, sub in enumerate(f.p):
        disasm(sub, f"{name}.{k}", depth + 1, out)
    return out


def calls(f, acc=None):
    """Globals read and method names (SELF keys) used anywhere in the chunk"""
    acc = acc if acc is not None else {"globals": set(), "methods": set(), "sets": set()}
    for ins in f.code:
        op, a, b, c, bx, _ = decode(ins)
        if OPS[op] == "GETGLOBAL":
            acc["globals"].add(f.k[bx])
        elif OPS[op] == "SETGLOBAL":
            acc["sets"].add(f.k[bx])
        elif OPS[op] == "SELF" and c >= MAXSTACK:
            acc["methods"].add(f.k[c - MAXSTACK])
    for sub in f.p:
        calls(sub, acc)
    return acc


def pack_proto(f, out):
    out += struct.pack("<5I", f.nups | f.nparams << 8 | f.maxstack << 16, f.line, len(f.code), len(f.k), len(f.p))
    out += struct.pack(f"<{len(f.code)}I", *f.code)
    for v in f.k:
        if isinstance(v, float):
            out += struct.pack("<If", 2, v)
        elif isinstance(v, str):
            b = v.encode("latin1")
            out += struct.pack("<II", 3, len(b)) + b + b"\0" * (-len(b) % 4)
        else:
            out += struct.pack("<I", 0)
    for sub in f.p:
        pack_proto(sub, out)


KEEP_GOALS = None  # --msb: goal ids whose NNNNNN_battle / _logic chunks are packed (all others are other maps')
FIRST = ["ai_define.lua", "goal_list.lua", "goal_list_dlc.lua", "logic_list.lua", "event_list.lua"]


def pack(out_path, sources):
    """sources: ['archive.luabnd.dcx' or 'archive.luabnd.dcx:a.lua,b.lua']; archive order is kept, files in the
    order given (all bytecode files of the archive, sorted, when no list)"""
    out, n = bytearray(), 0
    for src in sources:
        path, _, names = src.partition(":")
        files = {souls.basename(nm): d for _, nm, d in souls.bnd3(souls.read(path))}
        # definitions first (constants that the other chunks' REGISTER_* calls read at load time)
        order = names.split(",") if names else sorted(files, key=lambda n: (n not in FIRST, FIRST.index(n) if n in FIRST
                                                                           else 0, n))
        for name in order:
            if name not in files:
                raise SystemExit(f"{name} not in {path}")
            if files[name][:5] != b"\x1bLuaP":
                continue
            stem = name.split("_")[0]
            if KEEP_GOALS is not None and stem.isdigit() and name.endswith(("_battle.lua", "_logic.lua")) and \
                    int(stem) not in KEEP_GOALS:
                continue
            b = name.encode()
            out += struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)
            pack_proto(load(files[name]), out)
            n += 1
    with open(out_path, "wb") as fo:
        fo.write(b"DSRL" + struct.pack("<I", n) + out)
    print(f"{out_path}: {n} chunks, {len(out) + 8} bytes")


def msb_goals(game, map_id):
    """Battle / logic goal ids of the map's MSB enemy parts (their NpcThinkParam rows)"""
    import area
    P = souls.game_params(game)
    T, N = P["NpcThinkParam"], P["NpcParam"]
    ids = set()
    for model, npc, think, *_ in area.enemy_parts(souls.read(f"{game}/map/MapStudio/{map_id}.msb"), map_id):
        t = T.get(think) or T.get(N[npc][1]["aiThinkId"]) if npc in N else T.get(think)
        if t:
            ids |= {t[1]["battleGoalID"], t[1]["logicId"]}
    return ids


def main():
    if sys.argv[1] == "--pack":
        args = sys.argv[2:]
        if args[0] == "--msb":  # --msb <game> <map>: keep only the NNNNNN_battle / _logic chunks that map uses
            global KEEP_GOALS
            KEEP_GOALS = msb_goals(args[1], args[2])
            args = args[3:]
        pack(args[0], args[1:])
        return
    path = sys.argv[1]
    files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(path))}
    if "--calls" in sys.argv:
        for n in sorted(files):
            if files[n][:5] != b"\x1bLuaP":
                continue
            c = calls(load(files[n]))
            print(n, "sets", sorted(c["sets"]))
            print("   globals", sorted(c["globals"]))
            print("   methods", sorted(c["methods"]))
        return
    flt = sys.argv[2] if len(sys.argv) > 2 else ""
    for n in sorted(files):
        if flt in n:
            print(f"==== {n}")
            if files[n][:5] == b"\x1bLuaP":
                print("\n".join(disasm(load(files[n]))))
            else:  # .luagnl / .luainfo tables, or plain-text Lua source
                print(f"  (not bytecode, {len(files[n])} bytes) {files[n][:64]!r}")


if __name__ == "__main__":
    main()
