"""Named access to every DSR param table, with Paramdex row names and field descriptions.

    import params
    P = params.params("NpcParam")          # GameParam table (file stem)
    P[2750].turnVellocity, P[2750].name    # field by the paramdef's internal name; name = Paramdex row name or own
    params.params("m10_LightBank")         # DrawParam bank (param/DrawParam/aAA_DrawParam.parambnd.dcx)
    params.describe("NpcParam", "turnVellocity")   # (display name, description) from Paramdex Defs
    python3 tools/params.py NpcParam 2750  # print a row;  --check: Paramdex Defs vs the game's paramdefs

Values come from the game's own paramdefs (souls.paramdef / souls.param). Row names and field descriptions come from
Paramdex (github.com/soulsmods/Paramdex, DS1R/Names/*.txt `<id> <name>` lines and DS1R/Defs/*.xml; no licence file,
so read from the user's checkout at ext/Paramdex or $PARAMDEX, never copied into the repo); without it, rows keep
their own (Japanese) names. Duplicate row ids: the last row wins (souls.param); Table.all_rows has every copy."""
import functools, glob, os, sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamepath, souls

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def paramdex_dir():
    return os.environ.get("PARAMDEX") or os.path.join(ROOT, "ext", "Paramdex")


class Row:
    """One param row: fields as attributes (internal paramdef names), .id, .name, .own_name."""
    __slots__ = ("id", "name", "own_name", "fields")

    def __init__(self, rid, name, own_name, fields):
        self.id, self.name, self.own_name, self.fields = rid, name, own_name, fields

    def __getattr__(self, k):
        try:
            return self.fields[k]
        except KeyError:
            raise AttributeError(f"param row {self.id} has no field {k!r}") from None

    def __getitem__(self, k):
        return self.fields[k]

    def get(self, k, default=None):
        return self.fields.get(k, default)

    def __repr__(self):
        return f"<Row {self.id} {self.name!r}>"


class Table:
    """{row id: Row}; .ptype = paramdef type (NPC_PARAM_ST), .stem = file stem (NpcParam)."""

    def __init__(self, stem, ptype, rows, names):
        self.stem, self.ptype = stem, ptype
        self.all_rows = [Row(rid, names.get(rid) or own, own, vals) for rid, own, vals in rows]
        self.rows = {r.id: r for r in self.all_rows}

    def __getitem__(self, rid):
        return self.rows[rid]

    def get(self, rid, default=None):
        return self.rows.get(rid, default)

    def __contains__(self, rid):
        return rid in self.rows

    def __iter__(self):
        return iter(self.rows.values())

    def __len__(self):
        return len(self.rows)

    def find(self, text):
        """Rows whose name contains text (case-insensitive)."""
        t = text.lower()
        return [r for r in self if t in (r.name or "").lower()]


@functools.lru_cache(maxsize=None)
def _defs(game):
    return dict(souls.paramdef(d) for _, _, d in souls.bnd3(souls.read(f"{game}/paramdef/paramdef.paramdefbnd.dcx")))


@functools.lru_cache(maxsize=None)
def row_names(stem):
    """Paramdex DS1R/Names/<stem>.txt -> {row id: name}; {} if missing."""
    path = os.path.join(paramdex_dir(), "DS1R", "Names", stem + ".txt")
    out = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8-sig") as f:
            for line in f:
                rid, _, name = line.rstrip("\r\n").partition(" ")
                if rid.lstrip("-").isdigit():
                    out[int(rid)] = name
    return out


@functools.lru_cache(maxsize=None)
def _binders(game):
    """{stem: (bnd path, entry name)} for GameParam and every DrawParam bnd."""
    out = {}
    for path in [f"{game}/param/GameParam/GameParam.parambnd.dcx"] + sorted(glob.glob(f"{game}/param/DrawParam/*.parambnd.dcx")):
        for _, n, _ in souls.bnd3(souls.read(path)):
            out[souls.basename(n).rsplit(".", 1)[0]] = (path, n)
    return out


@functools.lru_cache(maxsize=None)
def params(stem, game=None):
    """Param table by file stem: GameParam (NpcParam, AtkParam_Pc, ...) or DrawParam bank (m10_LightBank)."""
    game = game or gamepath.game_dir()
    where = _binders(game).get(stem)
    if not where:
        raise KeyError(f"no param {stem!r}; known: {', '.join(sorted(_binders(game)))}")
    d = next(d for _, n, d in souls.bnd3(souls.read(where[0])) if n == where[1])
    ptype = souls.cstr(d, 0xC).strip()
    defs = _defs(game)
    if ptype not in defs:
        raise KeyError(f"{stem}: paramdef {ptype} not in the game's paramdefbnd")
    return Table(stem, ptype, souls.param_rows(d, defs[ptype]), row_names(stem))


def names(game=None):
    """Every param stem the game has."""
    return sorted(_binders(game or gamepath.game_dir()))


@functools.lru_cache(maxsize=None)
def _paramdex_defs():
    """{param type: {internal field name: (display name, description, def string)}} from Paramdex DS1R/Defs."""
    out = {}
    for path in sorted(glob.glob(os.path.join(paramdex_dir(), "DS1R", "Defs", "*.xml"))):
        root = ET.parse(path).getroot()
        fields = {}
        for f in root.iter("Field"):
            d = f.get("Def")
            name = d.split("=")[0].split()[1].split(":")[0].split("[")[0].strip()
            fields[name] = ((f.findtext("DisplayName") or "").strip(), (f.findtext("Description") or "").strip(), d)
        out[root.findtext("ParamType")] = fields
    return out


def describe(stem, field, game=None):
    """(display name, description) of a field from Paramdex Defs; ('', '') if unknown."""
    t = params(stem, game).ptype
    return _paramdex_defs().get(t, {}).get(field, ("", "", ""))[:2]


def check(game=None):
    """Paramdex Defs vs the game's paramdefs: same field names, types, bit sizes and order. -> [problems]."""
    game = game or gamepath.game_dir()
    problems, pdx = [], _paramdex_defs()
    for ptype, fields in sorted(_defs(game).items()):
        if ptype not in pdx:
            problems.append(f"{ptype}: no Paramdex def")
            continue
        theirs = list(pdx[ptype].items())
        if len(theirs) != len(fields):
            problems.append(f"{ptype}: {len(fields)} fields in the game, {len(theirs)} in Paramdex")
        for (name, itype, nbytes, bits, arr), (pname, (_, _, d)) in zip(fields, theirs):
            ptyp = d.split()[0]
            pbits = int(d.split(":")[1].split("=")[0]) if ":" in d.split("=")[0] else None
            if name != pname or (bits or None) != pbits:
                problems.append(f"{ptype}.{name}: game {itype}{':%d' % bits if bits else ''}, Paramdex {d}")
    return problems


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] == "--list":
        print("\n".join(names()))
    elif a[0] == "--check":
        n = 0
        for stem in names():  # every table loads and every row id gets a name where Paramdex has one
            T = params(stem)
            nm = row_names(stem)
            print(f"{stem:32s} {T.ptype:28s} rows {len(T.all_rows):5d}  Paramdex names {sum(1 for r in T.rows if r in nm):5d}/{len(T.rows)}")
            n += 1
        probs = check()
        print(f"{n} tables; Paramdex Defs vs game paramdefs: {len(probs)} differences")
        print("\n".join("  " + p for p in probs))
    else:
        T = params(a[0])
        rows = [T[int(x)] for x in a[1:]] or list(T)[:5]
        for r in rows:
            print(f"{T.stem} {r.id} {r.name}")
            for k, v in r.fields.items():
                dn, _ = describe(T.stem, k)
                print(f"   {k:28s} {v!r:>14}  {dn}")
