"""Per-format scanners of the coverage scan (tools/scan/scan.py): each runs the port's own reader over one game file
and records the variants it meets through rec(kind, value, status).

Status (worst wins when a variant is recorded twice):
  ok        the reader reads it and the port uses it (or there is nothing to port: a member type, a name)
  info      seen, no claim (a Havok member class, a file kind no reader opens)
  unported  the reader knows it, the port's runtime / exporter does not run it (an ESD env function without a handler,
            a Lua opcode lua0.c lacks, an FFX action data subtype export_effects skips, a TAE event type no exporter
            reads, an EMEVD instruction export_emevd marks unsupported)
  stub      EMEVD instructions export_emevd keeps as logged stubs
  unknown   the reader does not know it: souls.UnknownVariant raised, or souls.skipped() reported a pass-over
  error     the reader failed with another exception: a missing typed error, to fix in the reader
"""
import os
import re
import struct
import sys

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls  # noqa: E402
import support  # noqa: E402

SEVERITY = {"ok": 0, "info": 0, "unported": 1, "stub": 1, "unknown": 2, "error": 3}
# kinds a scanner records itself for every occurrence: a souls.skipped() report of the same kind only raises its status
SCANNED = {"emevd instruction", "msb part type", "msb event type", "msb region shape", "msb model type"}
DDS_READ = {"DXT1", "DXT3", "DXT5", "DX10:71", "DX10:72", "DX10:74", "DX10:75", "DX10:77", "DX10:78", "DX10:98",
            "DX10:99", "DX10:83", "DX10:84"}  # souls.dds_decode_rgba, export_normal BC5


class Recorder:
    def __init__(self):
        self.v = {}  # (kind, value) -> [status, count, example]
        self.where = ""

    def __call__(self, kind, value, status="ok", n=1):
        key = (kind, str(value))
        e = self.v.get(key)
        if e is None:
            self.v[key] = [status, n, self.where]
        else:
            if SEVERITY[status] > SEVERITY[e[0]]:
                e[0], e[2] = status, self.where
            e[1] += n

    def hook(self, kind, value):  # souls.SEEN
        self(kind, value, "unknown", 0 if kind in SCANNED else 1)

    def run(self, where, fn, *a):
        """fn(*a) under the reader hook; UnknownVariant / other exceptions recorded."""
        old, self.where = self.where, where
        souls.SEEN = self.hook
        try:
            fn(*a)
        except souls.UnknownVariant as e:
            self(e.kind, e.value, "unknown")
        except Exception as e:  # noqa: BLE001  a reader crash: what tools/scan exists to find
            self("reader error", f"{getattr(fn, '__name__', '?')}: {type(e).__name__}", "error")
        finally:
            souls.SEEN = None
            self.where = old

    def rows(self):
        return [[k, v, s, n, ex] for (k, v), (s, n, ex) in sorted(self.v.items())]


# ------------------------------------------------------------------------------------------------ shared context
class Ctx:
    """Game-wide tables the scanners need, loaded once per worker."""

    def __init__(self, game):
        self.game = game
        self._c = {}

    def get(self, name, fn):
        if name not in self._c:
            try:
                self._c[name] = fn()
            except Exception as e:  # noqa: BLE001
                print(f"scan: context {name}: {type(e).__name__}: {e}", file=sys.stderr)
                self._c[name] = None
        return self._c[name]

    def mtd(self):
        import mapgather
        return self.get("mtd", lambda: mapgather.mtd_params(self.game))

    def emevd_defs(self):
        import emevd
        return self.get("emedf", lambda: emevd.definitions(self.game)[0])

    def paramdefs(self):
        def load():
            out = {}
            for _, n, d in souls.bnd3(souls.read(os.path.join(self.game, "paramdef", "paramdef.paramdefbnd.dcx"))):
                ptype, fields = souls.paramdef(d)
                out[ptype] = fields
            return out
        return self.get("paramdef", load)

    def tae_template(self, obj):
        d = os.path.join(TOOLS, "..", "ext", "DSAnimStudio", "DSAnimStudioNETCore", "Res")
        f = os.path.join(d, "TAE.Template.DS1.OBJ.xml" if obj else "TAE.Template.DS1.xml")
        return self.get("tae_obj" if obj else "tae_chr", lambda: souls.tae_template(f) if os.path.exists(f) else None)


# ------------------------------------------------------------------------------------------------ formats
def _flver_layouts(b):
    """Vertex layouts of a FLVER2 as member lists [(type, semantic, index)] (the header walk of souls.flver)."""
    nd, nm, nb, nmesh, nvb = struct.unpack_from("<5i", b, 0x14)
    nfs, nl, _ = struct.unpack_from("<3i", b, 0x50)
    p = 0x80 + 0x40 * nd + 0x20 * nm + 0x80 * nb + 0x30 * nmesh + 0x20 * nfs + 0x20 * nvb
    out = []
    for i in range(nl):
        cnt, _, _, m_off = struct.unpack_from("<4i", b, p + 0x10 * i)
        out.append([struct.unpack_from("<5i", b, m_off + 0x14 * k)[2:] for k in range(cnt)])
    return out


def scan_flver(b, rec, ctx, where, role):
    """role: map / chr / obj / parts / other (a map piece's MTD is checked against the material classes)"""
    def go():
        F = souls.flver(b)
        ver = struct.unpack_from("<I", b, 8)[0]
        rec("flver version", hex(ver), "ok" if ver in souls.FLVER_VERSIONS else "unknown")
        for lay in _flver_layouts(b):
            sig = " ".join(f"{sem}:{typ:#x}:{si}" for typ, sem, si in lay)
            ok = all(souls.flver_member_read(typ, sem, si) for typ, sem, si in lay)
            rec("flver vertex layout", sig, "ok" if ok else "info")  # (the unread members: "flver vertex member")
        mp = ctx.mtd() or {}
        import material
        for m in F["meshes"]:
            mtd = m["mtd"]
            if mtd not in mp:
                rec("mtd missing from Mtd.mtdbnd", mtd, "unknown")
                continue
            bm = material.blend_mode(mp[mtd])
            rec(f"mtd g_BlendMode ({role})", "none" if bm is None else bm,
                "unknown" if bm is None else "ok" if bm in (0, 1, 2, 3, 4, 5) else "unported")
            if role == "map":
                cls = material.classify(mtd, m["diffuse"], params=mp[mtd])
                rec("material class (map)", material.NAMES[cls] if isinstance(cls, int) else cls)
                rec("mtd (map)", mtd)
    rec.run(where, go)


def scan_tpf(b, rec, where):
    def go():
        for name, d in souls.tpf(b).items():
            four = d[84:88].decode("latin1")
            if four == "DX10":
                four = f"DX10:{struct.unpack_from('<I', d, 128)[0]}"
            elif d[80:84] and not four.strip("\0"):
                four = f"rgb{struct.unpack_from('<I', d, 88)[0]}"
            rec("dds format", four, "ok" if four in DDS_READ else "unported")
    rec.run(where, go)


def scan_hkx(b, rec, where):
    def go():
        import havok_anim, havok_collision, havok_ragdoll
        h = souls.HkTag(b)
        names = {t.name for t, _, _ in h.items if t and t.name}
        for n in names:
            rec("havok class", n, "ok" if n in support.HAVOK_READ else "info")
        read = names & set(support.HAVOK_READ)
        if not read:
            anim = sorted(n for n in names if n.startswith("hka") and n.endswith("Animation"))
            rec("havok file not read", ",".join(anim) or "other", "unknown" if anim else "info")
            return
        if "hkaSkeleton" in names:
            havok_anim.skeleton(b)
        if "hkaSplineCompressedAnimation" in names:
            # havok_anim.animation without the frame evaluation: every block's tracks and quantizations
            A = h.type("hkaSplineCompressedAnimation")
            o = h.objects("hkaSplineCompressedAnimation")[0]
            nblocks, ntracks = h.get(o, A, "numBlocks", "i"), h.get(o, A, "numberOfTransformTracks", "i")
            _, do, dc = h.item(o, A, "data")
            _, bo, bc = h.item(o, A, "blockOffsets")
            data = h.data[do:do + dc]
            offs = list(struct.unpack_from(f"<{bc}I", h.data, bo)) + [dc]
            for i in range(nblocks):
                havok_anim._block(data[offs[i]:offs[i + 1]], ntracks)
            rec("havok animation", "spline")
        if "hkaRagdollInstance" in names:
            havok_ragdoll.ragdoll(b)
        if read & {"hkpStorageExtendedMeshShape", "CustomParamStorageExtendedMeshShape"}:
            havok_collision.hkx_meshes(b)
    rec.run(where, go)


def scan_tae(b, rec, ctx, where, kind):
    """kind: chr, obj (the DSAnimStudio OBJ template) or remo (cutscenes: no template of their own, no runtime)"""
    def go():
        tpl = ctx.tae_template(kind == "obj")
        ported = support.tae_types() if kind != "remo" else frozenset()
        for a in souls.tae(b).values():
            for t, _, _, _ in a["events"]:
                st = "ok" if t in ported else "unported" if tpl is None or t in tpl else "unknown"
                rec("tae event type" + ("" if kind == "chr" else f" ({kind})"), t, st)
    rec.run(where, go)


def scan_ffx(b, rec, where):
    def go():
        import ffx
        e = ffx.parse(b)
        for n in e.nodes.values():
            rec("ffx node type", n["t"])
            if n["t"] == 38:
                rec("ffx action type", n.get("ActionType"))
        for d in e.datas.values():
            rec("ffx action data", d["sub"], "ok" if d["sub"] in support.ffx_data() else "unported")
    rec.run(where, go)


def scan_emevd(b, rec, ctx, where):
    def go():
        import emevd
        E = emevd.parse(b, ctx.emevd_defs())
        cls = support.emevd_class()
        for ev in E.events:
            for ins in ev.instrs:
                c = cls.get((ins.bank, ins.id))
                st = "unknown" if ins.defn is None else {"real": "ok", "noop": "ok", "stub": "stub"}.get(c, "unported")
                rec("emevd instruction", f"{ins.bank}[{ins.id:02d}]", st)
    rec.run(where, go)


def scan_esd(b, rec, where, kind):
    """kind: player (c0000), enemy (enemyCommon), chr (other character ESDs: not exported), talk, menu"""
    def go():
        import esd
        e = esd.ESD(b)
        env, cmd = support.esd_handled().get(kind, (None, None))
        cmds, exprs = [], []
        for s in e.states.values():
            cmds += s["entry"] + s["exit"] + s["while"]
        for c in e.conds.values():
            cmds += c["pass"]
            exprs.append(c["eval"])
        for _, _, args in cmds:
            exprs += args
        for bank, cid, _ in cmds:
            rec(f"esd command ({kind})", f"{bank}[{cid}]", "ok" if cmd is not None and cid in cmd else "unported")
        for x in exprs:
            for fid in re.findall(r"f(-?\d+)\(", x):
                rec(f"esd env function ({kind})", int(fid), "ok" if env is not None and int(fid) in env else "unported")
    rec.run(where, go)


def scan_msb(b, rec, where):
    def go():
        m = souls.msb1(b)
        for p in m["parts"]:
            rec("msb part type", p["type"], "ok" if p["type_name"] else "unknown")
        for e in m["events"]:
            rec("msb event type", e["type"], "ok" if e["type_name"] else "unknown")
        for r in m["regions"]:
            rec("msb region shape", r["shape"], "ok" if isinstance(r["shape"], str) else "unknown")
        for mi in m["model_info"]:
            rec("msb model type", mi["type"], "ok" if mi["type_name"] else "unknown")
    rec.run(where, go)


def scan_lua(b, rec, where):
    def go():
        import lua50
        ops = support.lua_ops()

        def walk(f):
            for i in f.code:
                op = i & 0x3F
                if op >= len(lua50.OPS):
                    rec("lua opcode", op, "unknown")
                else:
                    rec("lua opcode", lua50.OPS[op], "ok" if lua50.OPS[op] in ops else "unported")
            for p in f.p:
                walk(p)
        walk(lua50.load(b))
    rec.run(where, go)


def scan_param(b, rec, ctx, where):
    def go():
        ptype = souls.cstr(b, 0xC).strip()
        defs = ctx.paramdefs() or {}
        if ptype not in defs:
            rec("param type", ptype, "unknown")
            return
        rec("param type", ptype)
        souls.param_rows(b, defs[ptype])
    rec.run(where, go)


def scan_fsb(path, rec, where):
    def go():
        import fsb4
        f = fsb4.FSB4(path)
        try:
            for s in f.samples:
                codec = "mpeg" if s.mode & fsb4.MODE_MPEG else "pcm16" if s.mode & 0x10 else f"mode {s.mode:#x}"
                rec("fsb codec", codec, "unknown" if codec.startswith("mode") else "ok")
        finally:
            f.f.close()
    rec.run(where, go)


def scan_fev(path, rec, where):
    def go():
        import fev
        f = fev.parse(path)
        rec("fev version", hex(f["version"]))
    rec.run(where, go)


# ------------------------------------------------------------------------------------------------ dispatch
def _role(rel):
    top = rel.replace("\\", "/").split("/")[0].lower()
    return {"map": "map", "chr": "chr", "obj": "obj", "parts": "parts"}.get(top, "other")


def scan_entry(name, d, rec, ctx, rel, depth=0):
    """One file's bytes (a game file or a binder entry), by its name."""
    low = name.lower()
    if low.endswith(".dcx"):
        low = low[:-4]
    where = rel if name == os.path.basename(rel) else f"{rel}:{souls.basename(name)}"
    ext = low.rsplit(".", 1)[-1] if "." in low else ""
    if ext in ("partsbnd", "objbnd", "chrbnd", "anibnd", "remobnd", "fgbnd", "rumblebnd", "ffxbnd", "luabnd",
               "talkesdbnd", "chresdbnd", "menuesdbnd", "parambnd", "paramdefbnd", "msgbnd", "nvmbnd", "mtdbnd",
               "shaderbnd") and depth < 3:
        entries = []
        rec.run(where, lambda: entries.extend(souls.bnd3(d)))
        rec("binder", ext)
        if ext in ("paramdefbnd", "mtdbnd", "shaderbnd"):
            return  # read through Ctx (paramdefs, mtd params); shaders: no reader
        for _, n, e in entries:
            scan_binder_entry(n or "", e, rec, ctx, rel, ext, depth + 1)
        return
    if ext == "flver":
        scan_flver(d, rec, ctx, where, _role(rel))
    elif ext == "tpf":
        scan_tpf(d, rec, where)
    elif ext == "msb":
        scan_msb(d, rec, where)
    elif ext == "emevd":
        scan_emevd(d, rec, ctx, where)
    elif ext == "emeld":
        import emevd
        rec.run(where, emevd.eld, d)
    elif ext == "emedf":
        import emevd
        rec.run(where, emevd.edf, d)
    elif ext == "esd":
        stem = souls.basename(name).split(".")[0].lower()
        scan_esd(d, rec, where, "player" if stem == "c0000" else "enemy" if stem == "enemycommon" else "chr")
    else:
        rec("no reader", ext or "(none)", "info")


def scan_binder_entry(n, e, rec, ctx, rel, parent, depth):
    low = n.lower()
    base = souls.basename(low)
    ext = base.rsplit(".", 1)[-1]
    where = f"{rel}:{souls.basename(n)}"
    if ext == "tae":
        scan_tae(e, rec, ctx, where, "remo" if parent == "remobnd" else
                 "obj" if parent == "objbnd" or base.startswith("o") else "chr")
    elif ext == "hkx":
        scan_hkx(e, rec, where)
    elif ext == "ffx":
        scan_ffx(e, rec, where)
    elif ext == "lua":
        scan_lua(e, rec, where)
    elif ext == "param":
        scan_param(e, rec, ctx, where)
    elif ext == "fmg":
        rec.run(where, souls.fmg, e)
    elif ext == "nvm":
        rec.run(where, souls.nvm, e)
    elif ext == "esd":
        stem = base.split(".")[0]
        kind = {"talkesdbnd": "talk", "menuesdbnd": "menu"}.get(parent)
        kind = kind or ("player" if stem == "c0000" else "enemy" if stem == "enemycommon" else "chr")
        scan_esd(e, rec, where, kind)
    elif ext == "chrtpfbhd":
        bdt = os.path.join(ctx.game, os.path.dirname(rel), base.rsplit(".", 1)[0] + ".chrtpfbdt")
        if os.path.exists(bdt):
            entries = []
            rec.run(where, lambda: entries.extend(souls.bnd3(e, souls.read(bdt))))
            for _, tn, t in entries:
                scan_tpf(t, rec, f"{where}:{souls.basename(tn or '')}")
        else:
            rec("no reader", "chrtpfbhd without bdt", "info")
    else:
        scan_entry(n, e, rec, ctx, rel, depth)


def scan_file(game, rel, ctx):
    """-> Recorder of one game file (relative path)."""
    rec = Recorder()
    rec.where = rel
    path = os.path.join(game, rel)
    low = rel.lower()
    if low.endswith((".tpfbdt", ".hkxbdt", ".chrtpfbdt")):
        return rec  # read with their header
    if low.endswith((".tpfbhd", ".hkxbhd")):
        bdt = path[:-3] + "bdt"
        entries = []
        rec.run(rel, lambda: entries.extend(souls.bxf3(path, bdt)))
        for _, n, e in entries:
            if low.endswith(".tpfbhd"):
                scan_tpf(e, rec, f"{rel}:{souls.basename(n or '')}")
            else:
                scan_hkx(e, rec, f"{rel}:{souls.basename(n or '')}")
        return rec
    if low.endswith(".fsb"):
        scan_fsb(path, rec, rel)
        return rec
    if low.endswith(".fev"):
        scan_fev(path, rec, rel)
        return rec
    if low.endswith((".exe", ".dll", ".bik", ".png", ".itl")):
        rec("no reader", low.rsplit(".", 1)[-1], "info")
        return rec
    d = []
    rec.run(rel, lambda: d.append(souls.read(path)))
    if d:
        if low.endswith(".dcx"):
            rec("dcx compression", "DFLT")
        scan_entry(os.path.basename(rel), d[0], rec, ctx, rel)
    return rec
