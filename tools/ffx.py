"""Reader for DSR effect files (FXR1, `.ffx` inside `sfx/FRPG_SfxBnd_*.ffxbnd.dcx`).

Layouts follow SoulsFormatsNEXT `Formats/FXR1/*` (Meowmaritus / SoulsFormats, cross-checked on every DSR bundle by
`python3 tools/ffx.py --check`); this is own code, DSR PC (64-bit, "wide") files only.

File layout (all offsets absolute; a "varint" is an i32 followed by 4 pad bytes 0 or 0xCDCDCDCD):
  0x00 'FXR\\0'  0x04 u32 0x10000  0x08 varint root node  0x10 i32 pointer table offset, i32 pointer count,
  i32 function count, i32 unk1, i32 unk2
  node       varint type, then per type (NODE_LAYOUT); type 133 = effect root: id, 7 x varint 0, unk,
             2 containers, varint state list offset, varint state count
  container  (40 B) varint node-pointer table, i32 count, i32 count, u8 flag1..3, u8 0, 4 pad, varint action data,
             varint modifier. Node-pointer = varint node offset. The pointed nodes are the action's parameters.
  state      (24 B) varint transitions, varint actions, i32 n transitions, i32 n actions
  transition (16 B) varint target state, varint evaluator node
  action     (48 B) varint action type, container
  actiondata i32 subtype, i32 size, varint n resources, varint resource ids, varint resource fields,
             varint parent container, then per subtype (DATA_LAYOUT)
  field      varint type, varint offset -> per type (FIELD_TYPES): curves are i32 n, f32 times[n], values[n](xN)

Python API: `parse(bytes) -> Effect` (`.id`, `.root`, `.states`, `.actions()`), `bundle(path) -> {id: bytes}`,
`textures(path) -> {tpf id: tpf bytes}`, `field_eval(field, t)`.
"""
import os, struct, sys

V = 8  # varint size in DSR's 64-bit files


class Reader:
    def __init__(self, b):
        self.b = b

    def i32(self, o):
        return struct.unpack_from("<i", self.b, o)[0]

    def f32(self, o):
        return struct.unpack_from("<f", self.b, o)[0]

    def var(self, o):
        pad = struct.unpack_from("<I", self.b, o + 4)[0]
        assert pad in (0, 0xCDCDCDCD), f"varint pad {pad:#x} at {o:#x}"
        return self.i32(o)


# ---- fields (FXField): type -> (name, reader) ----
def _ticks(r, o, n_val):
    n = r.i32(o)
    times = [r.f32(o + 4 + 4 * i) for i in range(n)]
    base = o + 4 + 4 * n
    vals = [[r.f32(base + 4 * (k * n + i)) for k in range(n_val)] for i in range(n)]
    return n, times, vals, base + 4 * n * n_val


def _f_seq(nv, extra=""):
    def rd(r, o):
        n, t, v, p = _ticks(r, o, nv)
        d = {"times": t, "values": [x[0] for x in v] if nv == 1 else v}
        for e in extra:
            d[{"m": "min", "M": "max", "r": "res"}[e]] = r.i32(p) if e == "r" else r.f32(p)
            p += 4
        return d
    return rd


FIELD_TYPES = {
    0: ("const_seq", _f_seq(1)), 4: ("seq", _f_seq(1)), 5: ("ranged_seq", _f_seq(1, "mM")),
    6: ("seq_ex", _f_seq(1, "r")), 7: ("ranged_seq_ex", _f_seq(1, "mMr")), 8: ("seq3", _f_seq(3)),
    9: ("ranged_seq3", _f_seq(3, "mM")), 12: ("rep_const_seq", _f_seq(1)), 16: ("rep_seq", _f_seq(1)),
    17: ("ranged_seq_b", _f_seq(1, "mM")), 20: ("seq3_new", _f_seq(3)),
    24: ("const", lambda r, o: {"value": r.f32(o)}),
    25: ("base_range", lambda r, o: {"base": r.f32(o), "min": r.f32(o + 4), "max": r.f32(o + 8)}),
    26: ("t26", lambda r, o: {"f": r.f32(o), "i": r.i32(o + 4)}),
    27: ("t27", lambda r, o: {"f": [r.f32(o), r.f32(o + 4), r.f32(o + 8)], "res": r.i32(o + 12)}),
    28: ("empty", lambda r, o: {}),
}


def read_field(r, o):
    t, off = r.var(o), r.var(o + V)
    name, rd = FIELD_TYPES[t]
    d = rd(r, off)
    d["t"], d["kind"] = t, name
    return d


def field_eval(f, t=0.0, rnd=0.5):
    """Value of a field at normalised/absolute time t (curves are linear between ticks, clamped at the ends).
    Ranged curves add min + rnd * (max - min) (ASSUMED: uniform random offset per particle)."""
    if f is None:
        return 0.0
    k = f["t"]
    if k == 24:
        return f["value"]
    if k == 25:
        return f["base"] + f["min"] + rnd * (f["max"] - f["min"])
    if k in (26, 27, 28):
        return 0.0
    ts, vs = f["times"], f["values"]
    if not ts:
        v = 0.0
    elif k in (12, 16) and ts[-1] > 0:
        t = t % ts[-1]
    if ts:
        if t <= ts[0]:
            v = vs[0]
        elif t >= ts[-1]:
            v = vs[-1]
        else:
            for i in range(len(ts) - 1):
                if ts[i] <= t <= ts[i + 1]:
                    a = (t - ts[i]) / ((ts[i + 1] - ts[i]) or 1)
                    v0, v1 = vs[i], vs[i + 1]
                    v = [x + (y - x) * a for x, y in zip(v0, v1)] if isinstance(v0, list) else v0 + (v1 - v0) * a
                    break
    if "min" in f and not isinstance(v, list):
        v += f["min"] + rnd * (f["max"] - f["min"])
    return v


# ---- action data (FXActionData) per subtype ----
# Codes: f f32, i i32, z i32 == 0, v varint == 0, V varint, F field, X DS1R extra block (5 fields, f32, 5 x i32).
_C4 = lambda p: [f"{p}R", f"{p}G", f"{p}B", f"{p}A"]
_EMIT = ["F", "Degree", "F", "Spread"]
DATA_LAYOUT = {
    27: ("point_sprite", "fDuration fDurationMult fDurationVariance z fRenderDepth z iTextureID iBlendMode v "
         "FScale FScaleMult " + " ".join("F" + c for c in _C4("Color1") + _C4("Color2")) +
         " iUnk8 iUnk9 iUnk10 fUnk11 X"),
    28: ("emit_cone", "FDegree FSpread FSpeed VDirectionMode"),
    29: ("emit_square", "FSize FDegree FSpread FDistributionMode FSpeed iDirectionMode"),
    30: ("emit_circle", "FRadius FDegree FSpread FSpeed fDistribution iDistributionMode VDirectionMode"),
    31: ("emit_sphere", "FRadius FDegree FSpread FSpeed iDistributionMode iMovementMode"),
    32: ("emit_box", "FSizeX FSizeY FSizeZ FDegree FSpread FSpeed iDistributionMode iMovementMode"),
    40: ("tracer", "fUnk0 fDuration iTextureID z iOrientation iBlendMode VTrailThickness FScale1X FScale1Y "
         "FScale2X FScale2Y fUnk7 fTrailMaxTime iTrailLength iUnk10 v FColorR FColorG FColorB FColorA iUnk12 "
         "iUnk13 v FUnk14 iUnk15 fUnk16 FTrailFadeIn FTrailFadeOut VTrailFadeMode X"),
    43: ("distortion", "fUnk1 z z iTextureID1 iTextureID2 iDistortionMode iShapeMode iOrientation VBlendMode "
         "FScaleX FScaleY FScaleZ FStretch FIntensity FWobbleSpeed FWobbleRadius FWaveSpeed FFlowSpeed "
         "FColorR FColorG FColorB FColorA VUnk8"),
    55: ("move_accel", "FGravity FFalloff FFalloffMult z fPhaseShift"),
    59: ("billboard", "fUnk1 z iTextureID z iOrientation iBlendMode FScaleX FScaleY FRotX FRotY FRotZ "
         "iFramesPerRow iFrameCount FFrameInitial FFrameTimeline FUVShiftX FUVShiftY FColorR FColorG FColorB "
         "FColorA iUnk8 iUnk9 z iUnk10 fUnk11 iUnk12 X"),
    61: ("model_a", "z z iModelID iOrientation iBlendMode iUnk3a VUnk3b FScaleX FScaleY FScaleZ z z iUnk5 "
         "fUnk6 v FUnk7 iFramesPerRow iFrameCount FFrameInitial FFrameTimeline FUVShiftX FUVShiftY FUVScrollX "
         "FUVScrollY FColorR FColorG FColorB FColorA iUnk11 iUnk12 z z X"),
    66: ("t66", "fDuration fUnk1 fUnk2 iAttached fUnk4 iUnk5 VUnk5b " +
         " ".join(f"FUnk6_{i}" for i in range(1, 19)) + " " +
         " ".join("F" + c for c in _C4("Color1") + _C4("Color2")) + " z z X"),
    70: ("multitex_billboard", "fDuration fDurationMult fLifetimeVariance iAttached fRenderDepth z iTextureID1 "
         "iTextureID2 iTextureID3 iOrientation iBlendMode iUnk8 FScale1X FScale1Y FScale2X FScale2Y FRotX FRotY "
         "FRotZ FRotSpeedX FRotSpeedY FRotSpeedZ " +
         " ".join(f"FTex{k}{a}" for k in (1, 2, 3) for a in ("OffX", "OffY", "ScrollX", "ScrollY")) + " " +
         " ".join("F" + c for c in _C4("Color1") + _C4("Color2")) +
         " iUnk10 z z z z z z z z iUnk11 iUnk12 iUnk13 fUnk14 iAlphaMode fSaturation iUnk17 X"),
    71: ("billboard_cluster", "fDuration fDurationMult fLifetimeVariance iAttached fRenderDepth z iTextureID "
         "iUnk7 iOrientation iBlendMode iUnk10 z FScale1X FScale1Y FScale2X FScale2Y FRotX FRotY FRotZ "
         "FRotSpeedX FRotSpeedY FRotSpeedZ iFramesPerRow iFrameCount FFrameInitial FFrameTimeline " +
         " ".join("F" + c for c in _C4("Color1") + _C4("Color2")) +
         " iA1 iA2 iA3 iA4 iUnk15 z z z z iUnk16 iUnk17 iUnk18 fUnk19 iAlphaMode fSaturation iUnk22 X"),
    84: ("move_wobble", "FGravity FFalloff FFalloffMult z fPhaseShift FTurnAngle VTurnInterval"),
    105: ("move_follow", "FGravity FFalloff FFalloffMult z fPhaseShift FTurnAngle VTurnInterval FFollowRate"),
    107: ("radial_blur", "fRenderDepth z iTextureID iDistortionMode FScaleX FScaleY FScaleZ FColorR FColorG "
          "FColorB FColorA"),
    108: ("model_cluster", "fDuration fDurationMult fLifetimeVariance iAttached fUnk5 z iModelID iOrientation "
          "iBlendMode iUnk8 VUnk8b FScale1X FScale1Y FScale1Z FScale2X FScale2Y FScale2Z FRotX FRotY FRotZ "
          "FRotSpeedX FRotSpeedY FRotSpeedZ iFramesPerRow iFrameCount FFrameInitial FFrameTimeline FUVOffX "
          "FUVOffY FUVScrollX FUVScrollY " + " ".join("F" + c for c in _C4("Color1") + _C4("Color2")) +
          " iUnk12 iUnk13 iUnk14 fUnk15 VUnk16 X"),
    117: ("t117", "FUnk1_1 FUnk1_2 FUnk1_3 FUnk1_4 FUnk1_5 FUnk1_6 iUnk2 iUnk3 FUnk4"),
}


def _read_layout(r, o, spec, d):
    for tok in spec.split():
        c, name = tok[0], tok[1:]
        if c == "f":
            d[name] = r.f32(o); o += 4
        elif c == "i":
            d[name] = r.i32(o); o += 4
        elif c == "z":
            assert r.i32(o) == 0, f"expected 0 at {o:#x} ({d.get('kind')})"; o += 4
        elif c == "v":
            assert r.var(o) == 0, f"expected varint 0 at {o:#x}"; o += V
        elif c == "V":
            d[name] = r.var(o); o += V
        elif c == "F":
            d[name] = read_field(r, o); o += 2 * V
        elif c == "X":
            for k in ("ModR", "ModG", "ModB", "ModA", "ModMult"):
                d["Color" + k] = read_field(r, o); o += 2 * V
            d["X_f"] = r.f32(o)
            d["X_i"] = [r.i32(o + 4 + 4 * k) for k in range(5)]
            o += 24
    return o


def read_action_data(r, o):
    sub, size = r.i32(o), r.i32(o + 4)
    nres, res_ids, res_fields = r.var(o + 8), r.var(o + 16), r.var(o + 24)
    kind, spec = DATA_LAYOUT[sub]
    d = {"sub": sub, "kind": kind, "size": size}
    end = _read_layout(r, o + 40, spec, d)
    d["_used"] = end - o
    d["resources"] = [(r.i32(res_ids + 4 * k), read_field(r, res_fields + 2 * V * k)) for k in range(nres)]
    return d


MODIFIER_LAYOUT = {0: "iUnk", 1: "iUnk1 iUnk2 iUnk3 fOffsetX fOffsetY fUnk6 iUnk7 fUnk8 fUnk9 fUnk10 fUnk11",
                   2: "fLifetime fUnk2 iUnk3 fUnk4", 3: "fUnk1 iUnk2 fUnk3 iUnk4", 4: "iUnk", 5: "iUnk",
                   6: "fUnk1 iUnk2", 7: "iUnk"}


# ---- nodes (FXNode): type -> layout ----
# Codes as above plus: T<n> tick list (varint times, varint values, varint count; n floats per tick, 'i' = int),
# N node pointer (varint), H indexed (i16 type, i16 index, 4 pad), A int array (varint offset, varint count),
# E effect-root body (type 133), L node offset list (type 134), C container offset (37/38).
NODE_LAYOUT = {
    1: "VValue", 2: "AValues", 3: "Ti", 5: "Ti", 6: "Ti", 7: "fValue p", 9: "Ti", 11: "T1", 12: "T1", 13: "T3",
    14: "T3", 19: "T4", 20: "T4", 21: "T12", 22: "T12", 27: "T4", 28: "T4", 29: "T12", 37: "VEffectID C VUnk",
    38: "VActionType C VUnk", 41: "VValue", 44: "H", 45: "H", 46: "H", 47: "H", 59: "H", 60: "H", 66: "H",
    68: "VSoundID", 69: "VSoundID", 70: "fTick p", 71: "H", 79: "iX iY", 81: "fX fY", 85: "fTick1 fTick2",
    87: "H", 89: "Ti one zero", 91: "T1 one NNode", 95: "T1 one NNode", 111: "VValue", 112: "", 113: "", 114: "H",
    115: "H", 120: "NNode1 NNode2", 121: "NNode1 NNode2", 122: "NNode1 NNode2", 123: "NNode1 NNode2",
    124: "NNode1 NNode2", 126: "NNode1 NNode2", 127: "NNode1 NNode2", 128: "NNode", 129: "", 130: "", 131: "",
    132: "", 133: "E", 134: "L", 136: "", 137: "VUnk1 VUnk2 VUnk3", 138: "VUnk1", 139: "VUnk1", 140: "iUnk1 iUnk2",
}


class Effect:
    def __init__(self, b):
        assert b[:4] == b"FXR\0" and struct.unpack_from("<I", b, 4)[0] == 0x10000, "not a DSR FXR1 file"
        assert struct.unpack_from("<I", b, 0xC)[0] in (0, 0xCDCDCDCD), "32-bit FXR1 (PTDE) not supported"
        self.r = Reader(b)
        self.nodes, self.containers, self.states, self.datas = {}, {}, {}, {}
        self.root = self.node(self.r.var(8))
        self.id = self.root.get("EffectID")

    # every object is memoised by offset; references are kept as offsets ("@node" etc.)
    def node(self, o):
        if not o:
            return None
        if o in self.nodes:
            return self.nodes[o]
        r = self.r
        t = r.var(o)
        d = {"t": t, "@": o}
        self.nodes[o] = d
        p = o + V
        for tok in NODE_LAYOUT[t].split():
            c, name = tok[0], tok[1:]
            if c == "T":
                to, vo, n = r.var(p), r.var(p + V), r.var(p + 2 * V)
                p += 3 * V
                d["times"] = [r.f32(to + 4 * k) for k in range(n)]
                if name == "i":
                    d["values"] = [r.i32(vo + 4 * k) for k in range(n)]
                else:
                    # "12" (colour-3 curves, node types 21 / 22 / 29): one RGBA per tick, then two more n x RGBA
                    # blocks of curve shape values (pi/4-like slopes; checked on all 7 such nodes in DSR), not 12 floats
                    # per tick as SoulsFormats reads them. The shape blocks are ignored (linear curves).
                    w = 4 if name == "12" else int(name)
                    vals = [[r.f32(vo + 4 * (w * k + j)) for j in range(w)] for k in range(n)]
                    d["values"] = [v[0] for v in vals] if w == 1 else vals
            elif c == "V":
                d[name] = r.var(p); p += V
            elif c == "f":
                d[name] = r.f32(p); p += 4
            elif c == "i":
                d[name] = r.i32(p); p += 4
            elif tok == "p":
                p += 4
            elif tok in ("one", "zero"):
                assert r.var(p) == (1 if tok == "one" else 0); p += V
            elif c == "N":
                d[name] = self.node(r.var(p)); p += V
            elif c == "H":
                d["itype"], d["index"] = struct.unpack_from("<hh", r.b, p); p += V
            elif c == "A":
                lo, n = r.var(p), r.var(p + V); p += 2 * V
                d["values"] = [r.i32(lo + 4 * k) for k in range(n)]
            elif c == "C":
                d["container"] = self.container(r.var(p)); p += V
            elif c == "E":
                d["EffectID"] = r.var(p); p += V
                for _ in range(7):
                    assert r.var(p) == 0; p += V
                d["Unk"] = r.var(p); p += V
                d["container1"] = self.container(p); p += 40
                d["container2"] = self.container(p); p += 40
                so, n = r.var(p), r.var(p + V)
                d["states"] = [self.state(so + 24 * k) for k in range(n)]
            elif c == "L":
                d["EffectID"] = r.var(p); d["Unk"] = r.var(p + V)
                lo, n = r.var(p + 2 * V), r.var(p + 3 * V)
                d["nodes"] = [self.node(r.i32(lo + 4 * k)) for k in range(n)]
        return d

    def container(self, o):
        if not o:
            return None
        if o in self.containers:
            return self.containers[o]
        r = self.r
        d = {"@": o}
        self.containers[o] = d
        to, n = r.var(o), r.i32(o + 8)
        d["flags"] = tuple(r.b[o + 16:o + 19])
        d["params"] = [self.node(r.var(to + V * k)) for k in range(n)]
        ad, mo = r.var(o + 24), r.var(o + 32)
        d["data"] = self.action_data(ad) if ad else None
        if mo:
            mt = r.i32(mo)
            m = {"t": mt}
            _read_layout(r, mo + 4, MODIFIER_LAYOUT[mt], m)
            d["modifier"] = m
        return d

    def action_data(self, o):
        if o not in self.datas:
            self.datas[o] = read_action_data(self.r, o)
        return self.datas[o]

    def state(self, o):
        if o in self.states:
            return self.states[o]
        r = self.r
        d = {"@": o}
        self.states[o] = d
        eo, ao, ne, na = r.var(o), r.var(o + V), r.i32(o + 16), r.i32(o + 20)
        d["actions"] = [{"type": r.var(ao + 48 * k), "container": self.container(ao + 48 * k + V)}
                        for k in range(na)]
        d["transitions"] = [{"target": r.var(eo + 16 * k), "eval": self.node(r.var(eo + 16 * k + V))}
                            for k in range(ne)]
        for tr in d["transitions"]:
            tr["target_state"] = self.state(tr["target"])
        return d

    def actions(self):
        """[(state index, action dict)] over the root's states."""
        out = []
        for si, s in enumerate(self.root.get("states", [])):
            for a in s["actions"]:
                out.append((si, a))
        return out

    def calls(self):
        """Child effect ids this effect spawns (EffectCall 37 / EffectCreate 134 / IntNode params of call actions)."""
        ids = set()
        for n in self.nodes.values():
            if n["t"] == 37:
                ids.add(n["EffectID"])
        return sorted(ids)


def parse(b):
    return Effect(b)


def bundle_entries(path):
    import souls
    return souls.bnd3(souls.read(path))


def bundle(path):
    """{effect id: ffx bytes} of one ffxbnd (id from the file name fNNNNNNN.ffx)."""
    out = {}
    for _, name, data in bundle_entries(path):
        base = name.replace("\\", "/").rsplit("/", 1)[-1]
        if base.endswith(".ffx"):
            out[int(base[1:-4])] = data
    return out


def textures(path):
    """{tpf stem ('s20030', 's00922_n'): tpf bytes} of one ffxbnd. FFX TextureID n is 's%05d' % n."""
    out = {}
    for _, name, data in bundle_entries(path):
        base = name.replace("\\", "/").rsplit("/", 1)[-1]
        if base.endswith(".tpf"):
            out[base[:-4]] = data
    return out


def sfx_dir(game=None):
    import gamepath
    return os.path.join(game or gamepath.game_dir(), "sfx")


def _check():
    """Parse every effect of every DSR bundle (asserts check pads / zero fields); report action data whose
    DATA_LAYOUT reads past its stored size."""
    import collections
    d = sfx_dir()
    bad, n, kinds = collections.Counter(), 0, collections.Counter()
    for f in sorted(os.listdir(d)):
        if not f.endswith(".ffxbnd.dcx"):
            continue
        for fid, b in bundle(os.path.join(d, f)).items():
            e = parse(b)
            n += 1
            for ad in e.datas.values():
                kinds[ad["sub"]] += 1
                if ad["_used"] > ad["size"]:  # size also covers the curve data stored after the fixed part
                    bad[(ad["sub"], ad["_used"], ad["size"])] += 1
    print(f"{n} effects; action data subtypes {dict(sorted(kinds.items()))}")
    print("size mismatches:", dict(bad) or "none")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    if sys.argv[1:] == ["--check"]:
        sys.exit(_check())
    # ffx.py <bundle stem e.g. m10_02> [effect id]: summary
    path = os.path.join(sfx_dir(), f"FRPG_SfxBnd_{sys.argv[1]}.ffxbnd.dcx")
    effs = bundle(path)
    for fid in ([int(sys.argv[2])] if len(sys.argv) > 2 else sorted(effs)):
        e = parse(effs[fid])
        acts = [(si, a["type"], (a["container"]["data"] or {}).get("kind")) for si, a in e.actions()]
        print(fid, "states", len(e.root.get("states", [])), "calls", e.calls(), "actions", acts)
