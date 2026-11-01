"""Area effects (DSR FFX particle systems + effect point lights) -> fx.bin for ps2/proto/src/fx.c.

    export_effects.py <game> <out fx.bin> <map_id> [--tone fx_tone.json]   (Makefile; tone = export_map's sidecar)
    export_effects.py <game> --describe <map_id>                           (instances / systems, no output)

What is placed (all DATA unless marked):
- MSB parts of the bonfire model (area.BONFIRE, o0200): BONFIRE_FX at the FLVER's dummy poly 100. The fire's frame is
  local Y = the dummy's upward vector, Z = its forward vector (MEASURED, capture particle buffers); the light's is
  local Z = upward, Y = forward (MEASURED light position); KINDLED lists reference-save kindling overrides. Effects 90000-90016 are the bonfire family
  in FRPG_SfxBnd_CommonEffects; 90001-90004 grow with kindling (ASSUMED: level 10 / 20 / 30 / 40), 90006 is the
  bonfire's light (colour (1, .686, .369) x flicker, radius 5 m). MEASURED (RenderDoc light buffer, Burg bonfire
  1012962): one light at the bonfire +0.42 m (= dummy 0.164 + the effect's offset 0.25 along local Z), end 5 m,
  begin 0, linear colour (10.55, 4.61, 1.17) = 30 x (A x c)^2.2 at A 0.62, inside the curve's 0.565..0.753.
  Every o0200 counts as lit (TECH_DEBT fx-3).
- MSB SFX events (effect_id at the region; frame = region rotation, local Y up), except ones an event script owns
  (entity_id != -1: fog walls etc., TECH_DEBT fx-4).
- MSB Light events: PointLightBank rows at their regions = static point lights (map bake only, static_lights()).
  MEASURED: their shader colour is 3.0 x lin(col, colA) (torch row 15 at Burg, all three channels).

FFX interpretation (tools/ffx.py has the layout; the semantics below are read off the data, ASSUMED where noted):
- A particle system is an EffectCall node of class 2023: params = start delay (Tick), particles per emission
  (IntSeq), total emissions (-1 = forever), emission interval (Tick), max alive (Int), then actions: appearance
  (data 71 / 70 / 59 / 27), emitter (28 cone, 29 square, 30 circle, 31 sphere, 32 box), movement (55 / 84 / 105),
  action 35 = offset (x, y, z) + rotation (deg x, y, z) of the system in its parent frame.
- Class 2101 nodes are transforms (their action 35) around children; 2117 / 2121 / 2020 groups; any other non-zero
  EffectCall id is another effect file, placed with the call's transform.
- Field type 25 = base x (min + max x U[0,1)) per particle: every (min, max) pair in the game centres on 1
  ((.75, .5), (.8, .4), (-1, 2), (0, 12.57) for angles). Curves are in seconds of particle age (they end at
  Duration).
- Size = Scale1 x Scale2(age) = the quad's full width / height (DSR's FRPG_Sfx_*_Particle.vpo corners at +-0.5;
  MEASURED against the capture particle buffers). Colour = Color1 x Color2(age) x ColorMod x ColorModMult(age) (DS1R
  extra block), linear HDR, tone mapped with the map bake's tone map (fx_tone.json).
- Emitters, DECODED from the exe (FXEmitterUtility.inl runtime, TECH_DEBT fx-6): direction = local +Z tilted by
  Degree x U^(1 + Spread) (Spread <= 0) or Degree x (1 - U^(1 - Spread)) and turned uniformly around it;
  DirectionMode 1 / 2 premultiply RotX(-90 / +90 deg), i.e. the parent frame's +Y / -Y (the system's own action-35
  rotation only places it); circle = the system's local XY plane, radius U^(1 + Distribution) or
  1 - U^(1 - Distribution), sqrt for DistributionMode 0. Movement Gravity g: acceleration -g m/s^2 along world up
  (ASSUMED units, fits the captured rise); movement 84 / 105 wobble is not decoded and left out.
- Blend: 6 = alpha blend, every other mode additive (ASSUMED from the textures: mode 0 / 7 sprites have opaque
  black backgrounds, mode 6 mist has soft alpha).
- Light (action 24): p0 colour curve RGBA (A = intensity), p2 radius; HDR = 30 x (A c)^2.2 (see above), DSR's
  falloff saturate(((end - d) / (end - begin))^3) x N.L (FRPG_Phn_* point-light loop).

fx.bin (version FX_VERSION, little endian):
  header 64 B: 'DSFX', u32 version, n_tex, n_sys, n_eff, n_inst, n_light, n_eff_ref, off_tex, off_sys, off_eff,
               off_inst, off_light, off_ref, 2 x u32 pad
  tex   16 B: u16 w, h, u16 frames per row, u16 frame count, u32 pixel offset (PSMT8), u32 CLUT offset (CSM1 CT32,
              alpha 0..0x80)
  sys  SYS_FMT: see fx.h FxSys (same order)
  eff   8 B: u16 first ref, u16 n sys refs, u16 first light, u16 n lights
  ref   4 B: u16 system index, u16 pad (an effect's systems are refs[first .. first + n))
  inst 60 B: u16 effect, u16 flags, f32 pos[3], f32 frame[9] (columns = local x, y, z in world), f32 pad[2]
  light LIGHT_FMT: see fx.h FxLight
"""
import json, math, os, random, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import area, dsr_light, ffx, souls

FX_VERSION = 1
BONFIRE_FX = (90001, 90006)   # burning bonfire at kindling level 10 + its light (see the docstring)
# Bonfires whose fire in the reference save is a higher kindling effect (MEASURED: particle counts in the Firelink
# capture, 15 glow / 77 embers / 12 flames = 90004's maxima 16 / 80 / 12). The save state proper is TECH_DEBT fx-3.
KINDLED = {("m10_02_00_00", "o0200_0000"): 90004}
CLASS_IDS = range(2000, 2200)  # EffectCall ids that are engine node classes, not effect files
PS_CLASS, XFORM_CLASS = 2023, 2101
APPEAR = {71, 70, 59, 27}
EMIT = {28: 4, 29: 5, 30: 1, 31: 2, 32: 3}  # data subtype -> fx.h FX_SHAPE_*
MOVE = {55, 84, 105}
N_CURVE = 8
TEX_MAX = int(os.environ.get("FX_TEX_MAX", 128))  # px, longest side of an exported effect texture (VRAM)
MAX_SYS_PER_EFFECT = 24
POINT_LIGHT_GAIN = 3.0     # MEASURED: shader colour / lin(PointLightBank row) (torch, Burg capture)
FFX_LIGHT_GAIN = 30.0      # MEASURED fit: shader colour / (A c)^2.2 of an FFX light (bonfire 90006, Burg capture)
CHAR_ALB = 0.35            # = export_map.CHAR_ALB: albedo the character light is fitted for
# Effect colour exposure. MEASURED fit (fx-2): DSR's Firelink bonfire (dsr-A shot, spot A) has orange flame edges
# (225, 120, 40) and a yellow core; one layer of 90001's flame (colour (1, .56, .25) x ColorMod (5, 2.79, 1.25)) at
# the texture's mean coverage gives (220, 118, 22) with exposure 1.0 through the area's tone map + ToneCorrect, two
# overlapping layers (255, 167, 45). The map's adapted exposure (4.5 at Firelink) turns every layer white.
FX_EXPOSURE = float(os.environ.get("FX_EXPOSURE", 1.0))

SYS_FMT = "<HBBHHffff3f3f3ff3ffff3f4f2f3f16f32B2ff4f2H3fB3x"  # = fx.h FxSys
LIGHT_FMT = "<H2x3ffff8f24Bf"


# ---- FFX tree walk ----
def _rot(rx, ry, rz):
    """3x3 (row-major) for rotations about X, then Y, then Z in degrees (ASSUMED order)."""
    cx, sx, cy, sy, cz, sz = (f(math.radians(a)) for a in (rx, ry, rz) for f in (math.cos, math.sin))
    X = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    Y = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    Z = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    mul = lambda a, b: [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return mul(Z, mul(Y, X))


def _apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def _compose(xf, off, rot):
    """xf = (R, t) parent frame; child placed at off (parent coords) with rotation rot."""
    R, t = xf
    t2 = [a + b for a, b in zip(t, _apply(R, off))]
    R2 = [[sum(R[i][k] * rot[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return R2, t2


ID_XF = ([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0.0, 0.0, 0.0])


def _offset(container):
    """Action-35 child of a container -> (offset, rotation matrix) or None."""
    for p in container["params"]:
        if p and p["t"] == 38 and p["ActionType"] == 35 and p["container"]:
            v = [q.get("Value", 0.0) if q else 0.0 for q in p["container"]["params"][:6]]
            v += [0.0] * (6 - len(v))
            return v[:3], _rot(*v[3:6])
    return None


def walk(e, load, out, xf=ID_XF, depth=0):
    """Collect ('ps', container, xf, parent xf) and ('light', container, xf, xf) from effect e (xf = the system's
    frame incl. its own action 35, parent xf = without it); load(id) -> Effect or None."""
    if depth > 6:
        return
    starts = [e.root.get("container2")] + [a["container"] for s in e.root.get("states", [])[:1] for a in s["actions"]]
    seen = set()

    def visit(c, xf):
        if c is None or c["@"] in seen:
            return
        seen.add(c["@"])
        for p in c["params"]:
            if not p:
                continue
            if p["t"] == 37 and p.get("container"):
                cid, cc = p["EffectID"], p["container"]
                if cid == PS_CLASS:
                    o = _offset(cc)
                    out.append(("ps", cc, _compose(xf, *o) if o else xf, xf))
                elif cid in CLASS_IDS or cid == 0:
                    o = _offset(cc) if cid == XFORM_CLASS else None
                    visit(cc, _compose(xf, *o) if o else xf)
                else:
                    child = load(cid)
                    if child:
                        walk(child, load, out, xf, depth + 1)
            elif p["t"] == 38 and p.get("container"):
                if p["ActionType"] == 24:
                    out.append(("light", p["container"], xf, xf))
                visit(p["container"], xf)

    for c in starts:
        visit(c, xf)


# ---- particle system -> record ----
def _f(d, k):
    v = d.get(k)
    return v if isinstance(v, dict) and v.get("t") not in (None, 28) else None


def _rand(f, default=1.0):
    """(base, a, b): value = base x (a + b U); constants and curves give (value at 0, 1, 0)."""
    if f is None:
        return default, 1.0, 0.0
    if f["t"] == 25:
        return f["base"], f["min"], f["max"]
    v = ffx.field_eval(f, 0.0)
    return (v if not isinstance(v, list) else v[0]), 1.0, 0.0


def _curve(f, life, default=1.0):
    if f is None:
        return [default] * N_CURVE
    out = []
    for i in range(N_CURVE):
        v = ffx.field_eval(f, life * i / (N_CURVE - 1))
        out.append(v[0] if isinstance(v, list) else v)
    return out


def _node_val(n, default=0.0):
    if n is None:
        return default
    if "Value" in n:
        return n["Value"]
    if "Tick" in n:
        return n["Tick"]
    if "values" in n and n["values"]:
        v = n["values"][0]
        return v[0] if isinstance(v, list) else v
    return default


def system(cc, xf, pxf, tone, tex_of, coverage):
    """Class-2023 container -> dict for SYS_FMT, or None when it has no supported appearance."""
    params = cc["params"]
    datas = {}
    for p in params:
        if p and p["t"] == 38 and p.get("container") and p["container"]["data"]:
            d = p["container"]["data"]
            datas.setdefault("appear" if d["sub"] in APPEAR else "emit" if d["sub"] in EMIT else
                             "move" if d["sub"] in MOVE else "other", d)
    ap = datas.get("appear")
    if ap is None:
        return None
    tid = ap.get("TextureID", ap.get("TextureID1", -1))
    tex = tex_of(tid, ap.get("FramesPerRow", 1) or 1, ap.get("FrameCount", 1) or 1)
    if tex is None:
        return None
    vals = [_node_val(p) for p in params[:11]]
    delay, per_emit, total, interval, max_alive = vals[5], vals[6], vals[7], vals[8], vals[10]
    if ap["sub"] == 59:  # single billboard: one particle, re-spawned when its curves loop
        delay, per_emit, total, interval, max_alive = 0.0, 1, -1, 0.0, 1
    life = ap.get("Duration") or 0.0
    if life <= 0:
        ends = [max(f["times"]) for f in ap.values() if isinstance(f, dict) and f.get("times")]
        life = max(ends) if ends else 1.0
    life_min = life * ap.get("DurationMult", 1.0) if ap.get("LifetimeVariance", 0) > 0 else life
    if interval <= 0:
        interval = life / max(1, max_alive)
    R, t = xf
    em = datas.get("emit") or {}
    shape = EMIT.get(em.get("sub"), 0)
    # Emitter (exe: FXEmitterUtility.inl runtime of FXClusterEmitter_*, see the docstring): positions in the
    # system's own frame R (circle / square: its local XY plane), directions from DirectionMode.
    ext = [0.0, 0.0, 0.0]
    if shape in (1, 2):
        ext = [_rand(_f(em, "Radius"), 0)[0]] * 3
    elif shape == 3:
        ext = [_rand(_f(em, k), 0)[0] / 2 for k in ("SizeX", "SizeY", "SizeZ")]
    elif shape == 5:
        sq = _rand(_f(em, "Size"), 0)[0] / 2
        ext = [sq, sq, 0.0]
    mode = em.get("DirectionMode", 0) if isinstance(em.get("DirectionMode", 0), int) else 0
    PR = pxf[0]
    axis = {1: _apply(PR, [0.0, 1.0, 0.0]), 2: _apply(PR, [0.0, -1.0, 0.0])}.get(mode, _apply(R, [0.0, 0.0, 1.0]))
    normal = _apply(R, [0.0, 0.0, 1.0])
    sp = _rand(_f(em, "Speed"), 0.0)
    spread = math.radians(_rand(_f(em, "Degree"), 0.0)[0])
    spread_k = _rand(_f(em, "Spread"), 0.0)[0]
    dist = em.get("Distribution", 0.0) if isinstance(em.get("Distribution"), float) else 0.0
    dist_sqrt = 1.0 if em.get("DistributionMode", 1) == 0 else 0.0
    mv = datas.get("move") or {}
    g = _rand(_f(mv, "Gravity"), 0.0)[0]
    drag = _rand(_f(mv, "Falloff"), 0.0)[0]
    # Movement 84 / 105 (FXClusterMovement_Yurayura / PartialFollow): TurnAngle / TurnInterval not decoded yet, so no
    # wobble (TECH_DEBT fx-6); only gravity and falloff apply.
    turn, turn_dt = 0.0, 0.0
    if ap["sub"] in (71, 70, 108):
        s1x, s1y = _f(ap, "Scale1X"), _f(ap, "Scale1Y") or _f(ap, "Scale1X")
        s2x, s2y = _f(ap, "Scale2X"), _f(ap, "Scale2Y") or _f(ap, "Scale2X")
    elif ap["sub"] == 59:
        s1x, s1y, s2x, s2y = _f(ap, "ScaleX"), _f(ap, "ScaleY") or _f(ap, "ScaleX"), None, None
    else:  # 27 point sprite
        s1x = s1y = _f(ap, "Scale")
        s2x = s2y = _f(ap, "ScaleMult")
    # Scale = the quad's full width / height (FRPG_Sfx_*_Particle.vpo corners at +-0.5 x size; MEASURED: Burg capture
    # particle buffer matches Scale1 x Scale2(age)); fx.c takes half extents.
    bx, ba, bb = _rand(s1x, 0.1)
    by = _rand(s1y, 0.1)[0]
    bx, by = bx * 0.5, by * 0.5
    kx, ky = _curve(s2x, life), _curve(s2y, life)
    if ap["sub"] == 59:
        c1 = [_curve(_f(ap, "Color" + ch), life) for ch in "RGBA"]
        c2 = [[1.0] * N_CURVE] * 4
    else:
        c1 = [_curve(_f(ap, "Color1" + ch), life) for ch in "RGBA"]
        c2 = [_curve(_f(ap, "Color2" + ch), life) for ch in "RGBA"]
    mod = [_curve(_f(ap, "ColorMod" + ch), life) for ch in "RGBA"]
    blend = 1 if ap.get("BlendMode") == 6 else 0
    mul = _curve(_f(ap, "ColorModMult"), life)
    rgba = []
    for i in range(N_CURVE):
        hdr = [c1[k][i] * c2[k][i] * mod[k][i] * mul[i] for k in range(3)]
        if blend:  # alpha blended: the colour is the pixel's
            out = tone(hdr)
        else:  # additive: tone map a typical texel's share (colour x mean texel coverage m), then back to colour
            m = coverage(tex)
            out = [o / m for o in tone([h * m for h in hdr])]
            if max(out) > 255 / 128:  # GS vertex colour <= 2x: scale the channels together to keep the hue (fx-2)
                out = [o * (255 / 128) / max(out) for o in out]
        a = max(0.0, min(1.0, c1[3][i] * c2[3][i] * mod[3][i]))
        rgba += [min(255, round(0x80 * o)) for o in out] + [min(0x80, round(0x80 * a))]
    r0 = _rand(_f(ap, "RotZ"), 0.0)
    rs = _rand(_f(ap, "RotSpeedZ"), 0.0)
    fr0 = _rand(_f(ap, "FrameInitial"), 0.0)
    tl = _f(ap, "FrameTimeline")
    rate = 0.0
    if tl and tl.get("times") and max(tl["times"]) > 0:
        rate = (tl["values"][-1] - tl["values"][0]) / (max(tl["times"]) - min(tl["times"]))
    return dict(tex=tex, blend=blend, shape=shape, per_emit=max(1, int(per_emit)), max_alive=max(1, int(max_alive)),
                life=life, life_min=life_min, interval=interval, delay=max(0.0, delay), total=int(total),
                org=t, axis=axis, ext=_apply([[abs(x) for x in r] for r in R], ext), spread=spread,
                spread_k=spread_k, dist=dist, dist_sqrt=dist_sqrt, normal=normal,
                speed=sp, accel=[0.0, -g, 0.0], drag=drag, turn=turn, turn_dt=turn_dt,
                size=(bx, by, ba, bb), kx=kx, ky=ky, rgba=rgba, rot=(r0[0] * r0[1], r0[0] * r0[2]),
                spin=rs, frame=(fr0[0] * fr0[1], fr0[0] * fr0[2]), rate=rate, fpr=0, nframes=0, frame_t=0)


def pack_sys(s, tex_fpr):
    fpr, nfr = tex_fpr
    return struct.pack(SYS_FMT, s["tex"], s["blend"], s["shape"], s["per_emit"], s["max_alive"], s["life"],
                       s["life_min"], s["interval"], s["delay"], *s["org"], *s["axis"], *s["ext"], s["spread"],
                       *s["speed"], s["drag"], s["turn"], s["turn_dt"], *s["accel"], s["size"][0], s["size"][1],
                       s["size"][2], s["size"][3], *s["rot"], *s["spin"], *(s["kx"] + s["ky"]), *s["rgba"],
                       *s["frame"], s["rate"], float(s["total"]), s["spread_k"], s["dist"], s["dist_sqrt"], fpr, nfr,
                       *s["normal"], 0)


# ---- lights ----
def ffx_light(cc, xf):
    """Action-24 container -> dict: offset, radius, begin, HDR colour samples over its loop period."""
    ps = cc["params"]
    col = ps[0] if ps else None
    rv = (ps[2] or {}).get("values") if len(ps) > 2 else None
    rad = max(v if not isinstance(v, list) else v[0] for v in rv) if rv else _node_val(ps[2] if len(ps) > 2 else None)
    begin = _node_val(ps[5], 0.0) if len(ps) > 5 else 0.0
    if col is None or rad <= 0:
        return None
    times = col.get("times") or [0.0]
    period = max(times) or 1.0
    samples = []
    for i in range(N_CURVE):
        t = period * i / N_CURVE
        v = None
        for j in range(len(times) - 1):
            if times[j] <= t <= times[j + 1]:
                a = (t - times[j]) / ((times[j + 1] - times[j]) or 1)
                v = [x + (y - x) * a for x, y in zip(col["values"][j], col["values"][j + 1])]
                break
        v = v or col["values"][-1]
        c, A = v[:3], v[3]
        samples.append([FFX_LIGHT_GAIN * max(0.0, A * x) ** 2.2 for x in c])
    return dict(off=xf[1], radius=rad, begin=max(0.0, begin), period=period, hdr=samples)


def cubic_light(pos, begin, end, rgb):
    """DSR point light (FRPG_Phn_* loop): rgb x saturate(((end - d) / (end - begin))^3) x N.L -> f(p, n)."""
    k = 1.0 / max(1e-6, end - begin)

    def f(p, n):
        v = [pos[i] - p[i] for i in range(3)]
        d = math.sqrt(sum(x * x for x in v)) or 1e-6
        if d >= end:
            return (0.0, 0.0, 0.0)
        att = min(1.0, (k * (end - d)) ** 3)
        nl = max(0.0, sum(n[i] * v[i] for i in range(3)) / d) if n else 1.0
        return [c * att * nl for c in rgb]
    return f


# ---- placement ----
class Area:
    """Everything placed in one area: [(effect id, pos, frame 3x3 columns)], static MSB lights."""

    def __init__(self, game, map_id):
        self.game, self.map_id = game, map_id
        self.M = souls.msb1(souls.read(os.path.join(game, "map", "MapStudio", map_id + ".msb")))
        keep = area.region(self.M, map_id)
        regs = {r["name"]: r for r in self.M["regions"]}
        self.inst, self.msb_lights = [], []
        up_fwd = bonfire_frame(game)
        for p in self.M["parts"]:
            if p["model"] == area.BONFIRE and keep(*p["pos"]):
                R = _rot_part(p["rot"])
                off, cols = up_fwd
                # Both sit at dummy 100. The light (90006) uses local Z = the dummy's up (MEASURED light height
                # +0.42 m); the fire uses local Y = up, Z = forward (MEASURED: Burg / Firelink capture particle
                # buffers, every layer rises along world Y from the dummy, group offset (0, -0.025, 0.05)).
                pos = [a + b for a, b in zip(p["pos"], _apply(R, off))]
                frame = [_apply(R, c) for c in cols]
                _, ny_fwd, nz_up = cols  # dummy frame columns: x, forward, up (model space)
                fire_frame = [_apply(R, c) for c in (_cross(nz_up, ny_fwd), nz_up, ny_fwd)]
                fire = KINDLED.get((map_id, p["name"]), BONFIRE_FX[0])
                self.inst.append((fire, pos, fire_frame, p["name"]))
                self.inst.append((BONFIRE_FX[1], pos, frame, p["name"]))
        for ev in self.M["events"]:
            r = regs.get(ev.get("region"))
            if r is None or not keep(*r["pos"]):
                continue
            if ev["type_name"] == "SFX" and ev.get("entity_id", -1) == -1 and ev.get("effect_id", -1) > 0:
                R = _rot_part(r.get("rot", (0, 0, 0)))
                self.inst.append((ev["effect_id"], list(r["pos"]), [[R[i][j] for i in range(3)] for j in range(3)],
                                  ev["name"]))
            elif ev["type_name"] == "Light" and ev.get("point_light_id", -1) >= 0:
                self.msb_lights.append((ev["point_light_id"], list(r["pos"]), ev["name"]))


def _rot_part(rot):
    """MSB part / region rotation (degrees; souls._euler_deg_xzy, as export_map.place) -> 3x3 row-major."""
    q = souls._euler_deg_xzy(*rot)
    cols = [souls._qrot(q, e) for e in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]
    return [[cols[j][i] for j in range(3)] for i in range(3)]


def bonfire_frame(game):
    """o0200's dummy poly 100: (position, [local x, y, z] in model space); local Z = upward, Y = forward."""
    fb = next(d for _, n, d in souls.bnd3(souls.read(os.path.join(game, "obj", area.BONFIRE + ".objbnd.dcx")))
              if n.endswith(".flver"))
    n = struct.unpack_from("<i", fb, 0x14)[0]
    for i in range(n):
        o = 0x80 + 0x40 * i
        pos = struct.unpack_from("<3f", fb, o)
        fwd = struct.unpack_from("<3f", fb, o + 16)
        ref = struct.unpack_from("<h", fb, o + 28)[0]
        up = struct.unpack_from("<3f", fb, o + 32)
        if ref == 100:
            nz = _norm(up)
            ny = _norm(fwd)
            nx = _norm([ny[1] * nz[2] - ny[2] * nz[1], ny[2] * nz[0] - ny[0] * nz[2], ny[0] * nz[1] - ny[1] * nz[0]])
            return list(pos), [nx, ny, nz]
    return [0.0, 0.0, 0.0], [[1, 0, 0], [0, 0, -1], [0, 1, 0]]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _norm(v):
    l = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / l for x in v]


class Library:
    """Effect files by id from the area's bundles (map, area prefix, CommonEffects, Patch overrides)."""

    def __init__(self, game, map_id):
        d = os.path.join(game, "sfx")
        self.paths = [os.path.join(d, f"FRPG_SfxBnd_{s}.ffxbnd.dcx")
                      for s in ("Patch", map_id[:6], map_id[:3], "CommonEffects")]
        self.paths = [p for p in self.paths if os.path.exists(p)]
        self._ffx, self._tpf, self.parsed = {}, {}, {}
        for p in self.paths:  # first bundle listed wins (Patch overrides CommonEffects)
            for _, name, data in ffx.bundle_entries(p):
                base = name.replace("\\", "/").rsplit("/", 1)[-1]
                if base.endswith(".ffx"):
                    self._ffx.setdefault(int(base[1:-4]), data)
                elif base.endswith(".tpf"):
                    self._tpf.setdefault(base[:-4], data)

    def effect(self, fid):
        if fid not in self.parsed:
            b = self._ffx.get(fid)
            self.parsed[fid] = ffx.parse(b) if b else None
        return self.parsed[fid]

    def texture(self, tid):
        t = self._tpf.get(f"s{tid:05d}")
        if t is None:
            return None
        dds = next(iter(souls.tpf(t).values()))
        return souls.dds_decode_rgba(dds, 1 << 12)


# ---- tone (the map bake's adapted tone map, written by export_map.py next to map.bin) ----
def write_tone(path, tone):
    """export_map.py hook: the lit map's adapted tone map for the effect colours."""
    if tone is None:
        return
    json.dump({"key": tone.key, "lo": tone.lo, "hi": tone.hi, "exposure": tone.exposure, "tc": tone.tc},
              open(path, "w"))


def load_tone(path):
    if path and os.path.exists(path):
        j = json.load(open(path))
        t = dsr_light.Tone({"grayKeyValue": j["key"], "minAdaptedLum": j["lo"], "maxAdapredLum": j["hi"]}, j["tc"],
                           j["exposure"])
        return t
    print("export_effects: no fx_tone.json, effect colours use the bright-scene exposure (re-export map.bin)")
    return None


def static_lights(game, map_id):
    """export_map.py hook: [f(position, normal) -> HDR irradiance] of the area's static point lights: MSB Light
    events (PointLightBank x POINT_LIGHT_GAIN) and the effect lights of placed effects (mean over their flicker)."""
    A = Area(game, map_id)
    banks = dsr_light.draw_banks(game, map_id[:3])["PointLight"]
    out = []
    for row, pos, _ in A.msb_lights:
        pl = banks.get(row)
        if pl and pl["colA"] > 0:
            rgb = [POINT_LIGHT_GAIN * dsr_light.lin(pl[f"col{c}"], pl["colA"]) for c in "RGB"]
            out.append(cubic_light(pos, pl["dwindleBegin"], pl["dwindleEnd"], rgb))
    lib = Library(game, map_id)
    for fid, pos, frame, _ in A.inst:
        e = lib.effect(fid)
        if e is None:
            continue
        found = []
        walk(e, lib.effect, found)
        for kind, cc, xf, pxf in found:
            if kind != "light":
                continue
            L = ffx_light(cc, xf)
            if L:
                p = [pos[i] + sum(frame[k][i] * L["off"][k] for k in range(3)) for i in range(3)]
                mean = [sum(s[c] for s in L["hdr"]) / len(L["hdr"]) for c in range(3)]
                out.append(cubic_light(p, L["begin"], L["radius"], mean))
    print(f"export_effects: {len(out)} static point lights ({len(A.msb_lights)} MSB Light events)")
    return out


# ---- textures ----
def quantize(img):
    """RGBA image -> (CSM1 CLUT, alpha 0..0x80; PSMT8 pixels) (same scheme as export_menu.quantize)."""
    from PIL import Image
    q = img.quantize(256, method=Image.Quantize.FASTOCTREE)
    pal = list(q.getpalette("RGBA") or [])[:1024]
    pal += [0] * (1024 - len(pal))
    clut = bytearray(1024)
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        r, g, b, a = pal[4 * i:4 * i + 4]
        clut[4 * j:4 * j + 4] = bytes([r, g, b, (a * 0x80 + 127) // 255])
    return bytes(clut), q.tobytes()


def export(game, map_id, out_path, tone_path=None, describe=False):
    from PIL import Image
    A = Area(game, map_id)
    lib = Library(game, map_id)
    tone = load_tone(tone_path) or dsr_light.Tone(
        {"grayKeyValue": 0.18, "minAdaptedLum": 0.05, "maxAdapredLum": 0.18}, None)  # fallback only
    texs, tex_index, covs = [], {}, []
    fx_tone = dsr_light.Tone.__new__(dsr_light.Tone)  # area tone map + ToneCorrect, effect exposure (FX_EXPOSURE)
    fx_tone.__dict__.update(tone.__dict__)
    fx_tone.exposure = FX_EXPOSURE

    def tex_of(tid, fpr, nfr):
        key = (tid, fpr, nfr)
        if key in tex_index:
            return tex_index[key]
        dec = lib.texture(tid) if tid >= 0 else None
        if dec is None:
            tex_index[key] = None
            return None
        w, h, px = dec
        img = Image.frombytes("RGBA", (w, h), bytes(px))
        s = min(1.0, TEX_MAX / max(w, h))
        # PSMT8 needs a buffer width of 128 (TBW 2): every effect texture is 128 wide, height by aspect
        tw, th = 128, max(8, min(256, 1 << round(math.log2(max(1, 128 * h / w)))))
        img = img.resize((tw, th), Image.LANCZOS)
        clut, pix = quantize(img)
        tex_index[key] = len(texs)
        texs.append((tw, th, fpr, nfr, pix, clut, tid))
        cov = [sum(p[:3]) / 765 * p[3] / 255 for p in img.getdata() if p[3] > 25]
        covs.append(max(0.05, sum(cov) / len(cov)) if cov else 1.0)  # mean rgb x alpha of the drawn texels
        return tex_index[key]

    effects, sys_recs, refs, lights, eff_of = [], [], [], [], {}
    for fid, pos, frame, name in A.inst:
        if fid in eff_of:
            continue
        e = lib.effect(fid)
        if e is None:
            print(f"export_effects: effect {fid} ({name}) not in the area's bundles, skipped")
            eff_of[fid] = None
            continue
        found = []
        walk(e, lib.effect, found)
        first, first_l = len(refs), len(lights)
        for kind, cc, xf, pxf in found:
            if kind == "ps" and len(refs) - first < MAX_SYS_PER_EFFECT:
                s = system(cc, xf, pxf, fx_tone, tex_of, lambda i: covs[i])
                if s:
                    refs.append(len(sys_recs))
                    sys_recs.append(s)
            elif kind == "light":
                L = ffx_light(cc, xf)
                if L:
                    lights.append(L)
        eff_of[fid] = len(effects)
        effects.append((first, len(refs) - first, first_l, len(lights) - first_l, fid))
    insts = [(eff_of[f], p, fr, n) for f, p, fr, n in A.inst if eff_of.get(f) is not None]
    if describe:
        for i, (f, n, fl, nl, fid) in enumerate(effects):
            print(f"effect {fid}: {n} systems, {nl} lights")
            for r in refs[f:f + n]:
                s = sys_recs[r]
                t = texs[s["tex"]]
                print(f"   tex s{t[6]:05d} {t[0]}x{t[1]} blend {s['blend']} shape {s['shape']} life {s['life']:.2f} "
                      f"every {s['interval']:.3f}s x{s['per_emit']} max {s['max_alive']} size {s['size'][0]:.3f}"
                      f"x{s['size'][1]:.3f} k {s['kx'][0]:.2f}->{s['kx'][-1]:.2f} speed {s['speed'][0]:.3f} "
                      f"org {[round(x, 3) for x in s['org']]} axis {[round(x, 2) for x in s['axis']]} "
                      f"rgba0 {s['rgba'][:4]} rgba4 {s['rgba'][16:20]}")
            for L in lights[fl:fl + nl]:
                print(f"   light off {[round(x, 3) for x in L['off']]} r {L['radius']} begin {L['begin']} "
                      f"hdr {[round(x, 2) for x in L['hdr'][0]]}..{[round(x, 2) for x in L['hdr'][N_CURVE // 2]]}")
        for f, p, fr, n in insts:
            print(f"inst {effects[f][4]} at {[round(x, 2) for x in p]} ({n})")
        return
    # character light units per effect light: the tone-mapped share for albedo CHAR_ALB on top of the scene level
    # (ASSUMED: scene irradiance key / exposure / lin(CHAR_ALB), grey)
    a = dsr_light.srgb_lin(CHAR_ALB)
    e0 = tone.key / max(1e-6, tone.exposure) / a
    light_recs = []
    for L in lights:
        amb = []
        for hdr in L["hdr"]:
            hi, lo = tone([a * (e0 + x) for x in hdr]), tone([a * e0] * 3)
            amb += [min(255, max(0, round(0x80 * (h - l) / CHAR_ALB))) for h, l in zip(hi, lo)]
        light_recs.append(struct.pack(LIGHT_FMT, 0, *L["off"], L["radius"], L["begin"], L["period"],
                                      *[sum(s[c] for s in L["hdr"]) / N_CURVE for c in range(3)], 0, 0, 0, 0, 0,
                                      *amb, 0.0))
    # layout
    HDR = 64
    tex_tab, pix_blob = b"", b""
    base_pix = None
    body_sys = b"".join(pack_sys(s, texs[s["tex"]][2:4]) for s in sys_recs)
    body_eff = b"".join(struct.pack("<4H", f, n, fl, nl) for f, n, fl, nl, _ in effects)
    body_ref = b"".join(struct.pack("<HH", r, 0) for r in refs)
    body_inst = b"".join(struct.pack("<HH3f9f2f", e, 0, *p, *[x for c in fr for x in c], 0.0, 0.0)
                         for e, p, fr, _ in insts)
    body_light = b"".join(light_recs)
    off_tex = HDR
    off_sys = off_tex + 16 * len(texs)
    off_eff = off_sys + len(body_sys)
    off_ref = off_eff + len(body_eff)
    off_inst = off_ref + len(body_ref)
    off_light = off_inst + len(body_inst)
    off_pix = (off_light + len(body_light) + 127) & ~127
    for tw, th, fpr, nfr, pix, clut, _ in texs:
        po = off_pix + len(pix_blob)
        pix_blob += clut
        pix_blob += pix
        pix_blob += b"\0" * (-len(pix_blob) % 128)
        tex_tab += struct.pack("<4H2I", tw, th, fpr, nfr, po + len(clut), po)
    head = struct.pack("<4s7I6I2I", b"DSFX", FX_VERSION, len(texs), len(sys_recs), len(effects), len(insts),
                       len(lights), len(refs), off_tex, off_sys, off_eff, off_inst, off_light, off_ref, 0, 0)
    assert len(head) == HDR
    blob = head + tex_tab + body_sys + body_eff + body_ref + body_inst + body_light
    blob += b"\0" * (off_pix - len(blob)) + pix_blob
    with open(out_path, "wb") as f:
        f.write(blob)
    vram = sum(tw * th + 1024 for tw, th, *_ in texs)
    print(f"{out_path}: {len(effects)} effects, {len(insts)} instances, {len(sys_recs)} particle systems, "
          f"{len(lights)} lights, {len(texs)} textures ({vram // 1024} KB VRAM), {len(blob)} bytes")


def main():
    game = sys.argv[1]
    if sys.argv[2] == "--describe":
        export(game, sys.argv[3], None, None, describe=True)
        return
    out, map_id = sys.argv[2], sys.argv[3]
    tone = sys.argv[sys.argv.index("--tone") + 1] if "--tone" in sys.argv else None
    export(game, map_id, out, tone)


if __name__ == "__main__":
    main()
