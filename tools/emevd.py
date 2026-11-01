"""DSR event scripts: EMEVD (event/*.emevd.dcx) decoded with the game's own instruction definitions
(event/common.emedf.dcx), event names from EMELD (*.emeld.dcx), English names from DarkScript3 when present.

    import emevd
    E = emevd.load("m10_02_00_00")             # events of a map ("common" for common.emevd)
    for ev in E.events: ev.id, ev.rest, ev.name, ev.instrs, ev.params
    for ins in ev.instrs: ins.bank, ins.id, ins.args (decoded values), ins.raw, ins.layer, ins.name
    python3 tools/emevd.py m10_02_00_00 [--jp] [--out f.txt]   # readable dump
    python3 tools/emevd.py --check                             # every emevd: every instruction decodes

Formats (32-bit little-endian DS1 layout, after SoulsFormatsNEXT EMEVD/EMELD, github.com/soulsmods/SoulsFormatsNEXT):
  EMEVD 'EVD\\0' 00 00 00 00, version 0xCC, file size, then (count, offset) u32 pairs: events, instructions,
    unused, layers, parameters, linked files, argument data (length), strings (length), u32 0.
    event 28 B: id, instr count, instr offset (rel. instructions), param count, param offset (rel. params),
      rest behaviour (0 default, 1 restart, 2 end), 0
    instruction 24 B: bank, id, args length, args offset (rel. argument data), layer offset (rel. layers, -1), 0
    parameter 20 B: instr index, target byte, source byte, byte count, unk   (event-arg substitution: X<src>_<n>)
    layer 20 B: 2, layer bits, 0, -1, 1
  EMEDF 'EDF\\0' 0, u16 0x65, u16 0xCC, file size, 9 (count, offset) pairs: classes (20 B: bank, instr count,
    instr offset rel. instr table, name offset, 0), instructions (20 B: id, arg count, arg offset rel. arg table,
    name offset, 0), args (36 B: name offset, type, enum offset rel. enum table or -1, default, min, max, increment,
    format, unk), 3 tables of an unused second class set, enums (16 B: name, value count, values offset), enum values
    (12 B: name, value, 0), UTF-16 strings (name offsets rel. strings).
  Arg types: 0 u8, 1 u16, 2 u32, 3 s8, 4 s16, 5 s32, 6 f32; each arg aligned to its own size, the block to 4.
  InitializeEvent-style instructions (2000[00], [06]) carry extra u32 event args after the defined ones.
EMELD 'ELD\\0' 0000, u16 0x65, u16 0xCC, size, events (count, offset), 2 unused pairs, strings (length, offset), 0, 0;
  event 12 B: id, name offset (rel. strings, UTF-16), 0."""
import functools, glob, json, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamepath, souls

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ARG_FMT = {0: "B", 1: "H", 2: "I", 3: "b", 4: "h", 5: "i", 6: "f"}
REST = {0: "Default", 1: "Restart", 2: "End"}


def _wstr(b, o):
    e = o
    while e + 1 < len(b) and b[e:e + 2] != b"\0\0":
        e += 2
    return b[o:e].decode("utf-16le", "replace")


class InstrDef:
    def __init__(self, bank, iid, name, args):
        self.bank, self.id, self.name, self.args = bank, iid, name, args  # args: [dict(name, type, enum, default, min, max)]
        self.name_en, self.arg_names_en, self.source = None, None, "common.emedf"

    def layout(self):
        """[(byte offset, struct fmt)] of the defined args and the aligned block size."""
        out, off = [], 0
        for a in self.args:
            fmt = ARG_FMT.get(a["type"], "I")
            size = struct.calcsize(fmt)
            off = (off + size - 1) // size * size
            out.append((off, fmt))
            off += size
        return out, (off + 3) // 4 * 4


def edf(b):
    """EMEDF bytes -> ({(bank, id): InstrDef}, {enum offset: (name, {value: name})}, class names {bank: name})."""
    assert b[:4] == b"EDF\0", "not an EMEDF"
    tabs = [struct.unpack_from("<II", b, 0x10 + 8 * i) for i in range(9)]
    (ncls, ocls), (_, oins), (_, oarg) = tabs[0], tabs[1], tabs[2]
    (nenum, oenum), (_, oval), (_, ostr) = tabs[6], tabs[7], tabs[8]
    s = lambda o: _wstr(b, ostr + o)
    enums = {}
    for i in range(nenum):
        name, cnt, voff, _ = struct.unpack_from("<iiii", b, oenum + 16 * i)
        vals = {}
        for k in range(cnt):
            vn, v, _ = struct.unpack_from("<iii", b, oval + voff + 12 * k)
            vals[v] = s(vn)
        enums[16 * i] = (s(name), vals)
    defs, classes = {}, {}
    for c in range(ncls):
        bank, n, ioff, cname, _ = struct.unpack_from("<5i", b, ocls + 20 * c)
        classes[bank] = s(cname)
        for k in range(n):
            iid, na, aoff, iname, _ = struct.unpack_from("<5i", b, oins + ioff + 20 * k)
            args = []
            for a in range(na):
                an, t, en, df, mn, mx, inc, fmt, unk = struct.unpack_from("<iiiiiiiii", b, oarg + aoff + 36 * a)
                args.append({"name": s(an), "type": t, "enum": enums.get(en) if en >= 0 else None,
                             "default": df, "min": mn, "max": mx})
            defs[(bank, iid)] = InstrDef(bank, iid, s(iname), args)
    return defs, enums, classes


def _json_lenient(path):
    """DarkScript3 emedf JSON has // comment lines and trailing commas."""
    with open(path, encoding="utf-8") as f:
        text = "\n".join(l for l in f.read().splitlines() if not l.lstrip().startswith("//"))
    return json.loads(re.sub(r",(\s*[\]}])", r"\1", text))


def darkscript_dir():
    return os.environ.get("DARKSCRIPT3") or os.path.join(ROOT, "ext", "DarkScript3")


@functools.lru_cache(maxsize=None)
def definitions(game=None):
    """The game's instruction definitions, with English names from DarkScript3's ds1-common.emedf.json (no licence
    file: read from the user's checkout at ext/DarkScript3 or $DARKSCRIPT3) where available."""
    game = game or gamepath.game_dir()
    defs, enums, classes = edf(souls.read(f"{game}/event/common.emedf.dcx"))
    path = os.path.join(darkscript_dir(), "DarkScript3", "Resources", "ds1-common.emedf.json")
    if os.path.exists(path):
        for c in _json_lenient(path)["main_classes"]:
            for i in c["instrs"]:
                d = defs.get((c["index"], i["index"]))
                if d is None:
                    # Used by DSR's scripts but missing from the shipped common.emedf (a PTDE-era table): take the arg
                    # types from DarkScript3 (same type codes); d.source says so.
                    d = defs[(c["index"], i["index"])] = InstrDef(c["index"], i["index"], i["name"], [
                        {"name": a["name"], "type": a["type"], "enum": None, "default": a.get("default", 0),
                         "min": a.get("min", 0), "max": a.get("max", 0)} for a in i["args"]])
                    d.source = "DarkScript3"
                d.name_en = i["name"]
                d.arg_names_en = [a["name"] for a in i["args"]]
    return defs, enums, classes


def eld(b):
    """EMELD bytes -> {event id: name}."""
    assert b[:4] == b"ELD\0", "not an EMELD"
    n, off = struct.unpack_from("<II", b, 0x10)
    sofs = struct.unpack_from("<I", b, 0x2C)[0]
    return {struct.unpack_from("<i", b, off + 12 * i)[0]: _wstr(b, sofs + struct.unpack_from("<i", b, off + 12 * i + 4)[0])
            for i in range(n)}


class Instr:
    __slots__ = ("bank", "id", "raw", "layer", "args", "extra", "defn")

    @property
    def name(self):
        d = self.defn
        return (d.name_en or d.name) if d else f"Unknown{self.bank}_{self.id:02d}"


class Event:
    __slots__ = ("id", "rest", "instrs", "params", "name")


class EMEVD:
    def __init__(self, events, linked, strings):
        self.events, self.linked, self.strings = events, linked, strings
        self.by_id = {e.id: e for e in self.events}


def decode_args(defn, raw):
    """Instruction arg bytes -> (values of the defined args, extra trailing u32s). Raises ValueError on a mismatch."""
    lay, size = defn.layout()
    if len(raw) < size and not (len(raw) == size - 4 + (size % 4 == 0) * 0 and False):
        raise ValueError(f"{defn.bank}[{defn.id:02d}] needs {size} B, has {len(raw)}")
    vals = [struct.unpack_from("<" + f, raw, o)[0] for o, f in lay]
    rest = raw[size:]
    if len(rest) % 4:
        raise ValueError(f"{defn.bank}[{defn.id:02d}] {len(rest)} trailing bytes")
    return vals, list(struct.unpack_from(f"<{len(rest) // 4}i", rest)) if rest else []


def parse(b, defs=None, names=None):
    """EMEVD bytes -> EMEVD; instructions decoded with defs (from definitions()) when given."""
    assert b[:4] == b"EVD\0" and b[4:8] == b"\0\0\0\0", "not a 32-bit LE DS1 EMEVD"
    t = [struct.unpack_from("<II", b, 0x10 + 8 * i) for i in range(8)]
    (nev, oev), (_, oins), _, (_, olay), (_, oprm), (nlink, olink), (_, oargs), (nstr, ostr) = t
    events = []
    for i in range(nev):
        eid, ni, io, npr, po, rest, _ = struct.unpack_from("<7i", b, oev + 28 * i)
        ev = Event()
        ev.id, ev.rest, ev.instrs, ev.name = eid, rest, [], (names or {}).get(eid)
        for k in range(ni):
            bank, iid, alen, aoff, loff, _ = struct.unpack_from("<6i", b, oins + io + 24 * k)
            ins = Instr()
            ins.bank, ins.id, ins.raw = bank, iid, b[oargs + aoff:oargs + aoff + alen] if alen else b""
            ins.layer = struct.unpack_from("<I", b, olay + loff + 4)[0] if loff != -1 else None
            ins.defn = (defs or {}).get((bank, iid))
            ins.args, ins.extra = (decode_args(ins.defn, ins.raw) if ins.defn else (None, None))
            ev.instrs.append(ins)
        ev.params = [struct.unpack_from("<5i", b, oprm + po + 20 * k) for k in range(npr)]
        events.append(ev)
    linked = list(struct.unpack_from(f"<{nlink}I", b, olink)) if nlink else []
    return EMEVD(events, linked, b[ostr:ostr + nstr])


def event_arg_types(ev):
    """{event-arg byte offset: struct fmt} of an event, from where its parameter substitutions land: a substitution
    copies event-arg bytes [src, src + n) into instruction arg bytes at dst, whose EDF type gives the format."""
    out = {}
    for ii, dst, src, n, _ in ev.params:
        ins = ev.instrs[ii] if 0 <= ii < len(ev.instrs) else None
        if not ins or not ins.defn:
            continue
        lay, _ = ins.defn.layout()
        for off, fmt in lay:
            if off == dst and struct.calcsize(fmt) == n:
                out[src] = fmt
    return out


def type_extra_args(E, *others):
    """Re-type InitializeEvent event args (defined 'Parameters' word + extra words, raw s32) as the called event reads
    them: each substitution source gives a width and type (u8 / u16 / f32 ...); untyped 4-byte slots stay s32, other
    untyped bytes are padding. The called event is looked up in E, then in others (common.emevd)."""
    tables = [E] + list(others)
    cache = {}
    for ev in E.events:
        for ins in ev.instrs:
            if not ins.args or (ins.bank, ins.id) != (2000, 0) or not ins.extra:
                continue
            target = ins.args[1]
            if target not in cache:
                tev = next((t.by_id[target] for t in tables if target in t.by_id), None)
                cache[target] = event_arg_types(tev) if tev else {}
            types = cache[target]
            blob = struct.pack(f"<{1 + len(ins.extra)}i", ins.args[2], *ins.extra)
            vals, o = [], 0
            while o < len(blob):
                fmt = types.get(o)
                if fmt is None and o % 4 == 0 and not any(o + k in types for k in range(1, 4)):
                    fmt = "i"
                if fmt is None:
                    o += 1  # padding after a narrow arg
                    continue
                vals.append(struct.unpack_from("<" + fmt, blob, o)[0])
                o += struct.calcsize(fmt)
            ins.args = ins.args[:2] + vals[:1]
            ins.extra = vals[1:]


def load(map_id, game=None):
    game = game or gamepath.game_dir()
    defs, _, _ = definitions(game)
    names = {}
    if os.path.exists(f"{game}/event/{map_id}.emeld.dcx"):
        names = eld(souls.read(f"{game}/event/{map_id}.emeld.dcx"))
    E = parse(souls.read(f"{game}/event/{map_id}.emevd.dcx"), defs, names)
    others = [] if map_id == "common" else [_common(game)]
    type_extra_args(E, *others)
    return E


@functools.lru_cache(maxsize=None)
def _common(game):
    return parse(souls.read(f"{game}/event/common.emevd.dcx"), definitions(game)[0])


def _fmt_val(v, a, jp):
    if isinstance(v, float):
        v = round(v, 4)
    if a and a.get("enum") and v in a["enum"][1] and jp:
        return f"{v}:{a['enum'][1][v]}"
    return repr(v)


def dump(E, jp=False):
    """Readable text: one block per event, one line per instruction; X<src>_<n> marks an event-arg substitution."""
    out = []
    for ev in E.events:
        out.append(f"Event {ev.id} {REST.get(ev.rest, ev.rest)}" + (f"  # {ev.name}" if ev.name else ""))
        subst = {(p[0], p[1]): f"X{p[2]}_{p[3]}" for p in ev.params}
        for k, ins in enumerate(ev.instrs):
            d = ins.defn
            if d is None or ins.args is None:
                out.append(f"    {ins.bank}[{ins.id:02d}] ?({ins.raw.hex()})")
                continue
            lay, _ = d.layout()
            names = (d.arg_names_en if d.arg_names_en and not jp and len(d.arg_names_en) == len(d.args) else
                     [a["name"] for a in d.args])
            parts = []
            for (off, _), a, n, v in zip(lay, d.args, names, ins.args):
                parts.append(f"{n}={subst.get((k, off), _fmt_val(v, a, jp))}")
            parts += [repr(round(x, 4)) if isinstance(x, float) else str(x) for x in ins.extra]
            lyr = f" layer {ins.layer:#x}" if ins.layer is not None else ""
            out.append(f"    {ins.bank}[{ins.id:02d}] {(d.name if jp else d.name_en or d.name)}({', '.join(parts)}){lyr}")
    return "\n".join(out)


def check(game=None):
    """Every emevd in event/: parse + decode every instruction. -> (counts, problems)."""
    game = game or gamepath.game_dir()
    defs, _, _ = definitions(game)
    counts, problems = {"files": 0, "events": 0, "instrs": 0, "named_en": 0, "defs_from_darkscript3": 0}, []
    unknown = {}
    for path in sorted(glob.glob(f"{game}/event/*.emevd.dcx")):
        mid = os.path.basename(path).split(".")[0]
        try:
            E = parse(souls.read(path), defs)
        except Exception as e:
            problems.append(f"{mid}: {type(e).__name__}: {e}")
            continue
        counts["files"] += 1
        counts["events"] += len(E.events)
        for ev in E.events:
            for ins in ev.instrs:
                counts["instrs"] += 1
                if ins.defn is None:
                    unknown[(ins.bank, ins.id)] = unknown.get((ins.bank, ins.id), 0) + 1
                else:
                    counts["named_en"] += bool(ins.defn.name_en)
                    counts["defs_from_darkscript3"] += ins.defn.source != "common.emedf"
    for (bk, i), c in sorted(unknown.items()):
        problems.append(f"instruction {bk}[{i:02d}] not in common.emedf ({c} uses)")
    return counts, problems


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] == "--check":
        counts, probs = check()
        print(counts)
        print("\n".join(probs) or "no problems")
        sys.exit(1 if probs else 0)
    text = dump(load(a[0]), jp="--jp" in a)
    if "--out" in a:
        with open(a[a.index("--out") + 1], "w") as f:
            f.write(text + "\n")
    else:
        print(text)
