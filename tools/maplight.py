"""The map exporter's light and fog: DrawParam banks, tone maps, character light, the fog section (2026-11-12, code moved as it was; tools/export_map.py runs it)."""
import sys
import math, os, struct
import souls
import dsr_light
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapcfg


def draw_params(game, area):
    """param/DrawParam/<area>_DrawParam.parambnd -> ({row: LightBank fields}, {row: FogBank fields}, {row: ToneMapBank
    fields}) of the area."""
    defs = {}
    for _, n, d in souls.bnd3(souls.read(os.path.join(game, "paramdef", "paramdef.paramdefbnd.dcx"))):
        t, f = souls.paramdef(d)
        defs[t] = f
    banks = {souls.basename(n): d for _, n, d in
             souls.bnd3(souls.read(os.path.join(game, "param", "DrawParam", f"a{area[1:3]}_DrawParam.parambnd.dcx")))}
    light = {r: v for r, (_, v) in souls.param(banks[f"{area}_LightBank.param"], defs["LIGHT_BANK"]).items()}
    fog = {r: v for r, (_, v) in souls.param(banks[f"{area}_FogBank.param"], defs["FOG_BANK"]).items()}
    tone = {r: v for r, (_, v) in souls.param(banks[f"{area}_ToneMapBank.param"], defs["TONE_MAP_BANK"]).items()}
    return light, fog, tone


def part_draw_ids(msb, n=2):
    """MSB1 parts in file order -> [(LightID, FogID, ...)] from each part's entity data: the first n of the s8 fields
    after the entity id (LightID, FogID, ScatterID, LensFlareID, ShadowID, DofID, ToneMapID, ToneCorrectID)."""
    def table(pos):
        _, _, count = struct.unpack_from("<iii", msb, pos)
        offs = struct.unpack_from(f"<{count - 1}i", msb, pos + 12)
        return offs, struct.unpack_from("<i", msb, pos + 12 + 4 * (count - 1))[0]
    _, nxt = table(0)
    for _ in range(2):  # events, regions
        _, nxt = table(nxt)
    out = []
    for o in table(nxt)[0]:
        ent = struct.unpack_from("<i", msb, o + 0x58)[0]
        out.append(struct.unpack_from(f"<{n}b", msb, o + ent + 4))
    return out


def fog_rgb(row, tone):
    """FogBank colour as the PS2 shows it. DSR's map shaders (FRPG_Phn_*.fpo, decompiled from a RenderDoc capture
    at Firelink) take the lit HDR colour to gamma space (pow 1/2.2), lerp it towards gFC_FogCol = pow(col / 255 x
    colA / 100, 2.2) (exe FUN_1401c8b00; measured 0.170 for row 20) by saturate(max x saturate((z - begin) / (end -
    begin))), apply the light scattering (dsr_light.Scatter), go back to linear (pow 2.2) and only then tone map. The
    GS mixes after the vertex-baked tone map, so its fog colour is the constant scattered at half the fog's end
    distance (ASSUMED reference distance, view across the sun), taken back to linear and tone mapped."""
    F = [dsr_light.lin(row[f"col{c}"], row["colA"]) for c in "RGB"]
    if mapcfg.SCATTER[0]:
        F = mapcfg.SCATTER[0].gamma(F, max(0.0, row["fogEndZ"] / 2), 0.0)
    return [round(255 * v) for v in tone([max(0.0, x) ** 2.2 for x in F])]


def area_tone(tone_rows, banks, t, c):
    """The baked map's tone map for (ToneMapID, ToneCorrectID) (auto exposure from gather), else a new one."""
    if (t, c) not in mapcfg.TONES:
        mapcfg.TONES[t, c] = dsr_light.Tone(tone_rows.get(t, tone_rows[min(tone_rows)]), banks["ToneCorrect"].get(c))
    return mapcfg.TONES[t, c]


def adapted(tone, light_row, faces):
    """A copy of tone with its exposure adapted to an area lit by light_row (no baked scene there: the luminance of
    a 0.3 albedo averaged over all normals stands in for DSR's screen average)."""
    f = dsr_light.light_fn(light_row)
    ns = [(math.sqrt(1 - (1 - 2 * (i + 0.5) / 16) ** 2) * math.cos(i * 2.4), 1 - 2 * (i + 0.5) / 16,
           math.sqrt(1 - (1 - 2 * (i + 0.5) / 16) ** 2) * math.sin(i * 2.4)) for i in range(16)]
    e = [f(n, faces) for n in ns]
    lum = sum(dsr_light.LUMA[c] * dsr_light.srgb_lin(0.3) * x[c] for x in e for c in range(3)) / len(e)
    t = dsr_light.Tone.__new__(dsr_light.Tone)
    t.__dict__.update(tone.__dict__)
    t.adapt(lum)
    return t


def char_light(row, faces, tone, sun=None, shadow=False):
    """Skinned characters on the PS2 (vu1skin.vsm): colour = amb + sun x max(n . L, 0) in 0x80 units for texture
    modulation. Fitted to DSR's light (LightBank row, light probe, tone map) for a mid-grey albedo CHAR_ALB: amb =
    the mean output over normals facing away from L, amb + sun = the output facing L. Returns (amb, sun, L)."""
    f0 = dsr_light.light_fn(row)
    k = mapcfg.SUN_FLOOR[0] if shadow else 1.0  # characters stand in sun this often: the bake's shadowing on average
    near = mapcfg.SCATTER[0].near_factor() if mapcfg.SCATTER[0] else [1.0, 1.0, 1.0]  # scattering at d = 0 (dsr_light.Scatter)
    f = lambda n, faces: [(a * (1 - mapcfg.SHADOW_AMB * (1 - k)) + b * k) * m for a, b, m in zip(*f0(n, faces, split=True), near)]
    L = sun or max(((dsr_light.light_dir(row[f"degRotX_{i}"], row[f"degRotY_{i}"]), dsr_light.lin(255, row[f"colA_{i}"]))
                    for i in "012"), key=lambda d: d[1])[0]
    a = dsr_light.srgb_lin(mapcfg.CHAR_ALB)
    out = lambda n: tone([a * e for e in f(n, faces)])
    away = []
    for i in range(32):  # Fibonacci sphere, the half facing away from L
        y = 1 - 2 * (i + 0.5) / 32
        r, ph = math.sqrt(1 - y * y), i * 2.39996
        n = (r * math.cos(ph), y, r * math.sin(ph))
        if n[0] * L[0] + n[1] * L[1] + n[2] * L[2] <= 0:
            away.append(out(n))
    amb = [sum(o[c] for o in away) / len(away) / mapcfg.CHAR_ALB for c in range(3)]
    full = [v / mapcfg.CHAR_ALB for v in out(L)]
    return amb, [max(0.0, full[c] - amb[c]) for c in range(3)], L


def probe_id(event):
    """Collision part env_event (MSB Environment event: 'GIマップスポット29', 'GIEL h1000') -> light probe number
    (GI_EnvDif_<map>_0029 / _1000): the event name's trailing number."""
    import re
    m = re.findall(r"\d+", event or "")
    return int(m[-1]) if m else -1


def fog_section(game, map_id, main_fog):
    """Area draw-param rows + the row of every collision triangle, for map.c's area switch: DSR takes the draw
    params of the collision part under the player (MSB fog / light / tone map / tone correct / point light
    (lantern) ids and its GI probe, env_event). One row per distinct set; row 0 = the most common set with the main
    FogID. Collision triangles are numbered like export_collision.area_collision (collision.bin's set), so
    col_floor's hit index is the index here. Each row: the FogBank row, the character light (char_light: the
    collision's LightID row, probe, tone map) and the player-attached point light (PointLightBank[lantern_id], e.g.
    Firelink 20 'B2拠点', Burg 10/11/12 player outdoors / indoors / shade), which lights characters by their
    distance to the player. Returns (section bytes, [(row key, fog fields, char light)], tone map per row key).
    Section: u32 rows, collision tris, 0, 0; rows (48 B) {f32 begin, end, max density; u8 fog r, g, b, 0;
    u8 ambient r, g, b, 0; u8 sun r, g, b, 0; s8 sun direction x, y, z (x 127), u8 point light begin (0.1 m);
    u8 point light r, g, b (added to the ambient at full strength), u8 point light end (0.1 m); 48 B: u8 env
    reflection up r, g, b, down r, g, b, specular light r, g, b (0x80 = 1.0); s8 specular light direction x, y, z
    (x 127); u8 far depth of field begin, end (m), strength (%), 0}; u8 row per triangle. The DoF bytes: DofBank
    row of the collision part's DofID (farDofBegin / farDofEnd / farDofMul; zero: none), for main.c's far blur."""
    import export_collision
    msb = souls.read(f"{game}/map/MapStudio/{map_id}.msb")
    M = souls.msb1(msb)
    light_rows, fog_rows, tone_rows = draw_params(game, map_id[:3])
    banks = dsr_light.draw_banks(game, map_id[:3])
    probes = dsr_light.load_probes(game, map_id)
    spec_probes = dsr_light.load_spec_probes(game, map_id)
    coll = {p["name"]: p for p in M["parts"] if p["type"] == 5}
    key_of = lambda p: (p["fog_id"] if p["fog_id"] in fog_rows else main_fog, p["light_id"], p["tone_map_id"],
                        p["tone_correct_id"], p["lantern_id"], probe_id(p.get("env_event")), p["dof_id"])
    subs = export_collision.area_collision(game, map_id)[0]  # collision.bin's triangles (area region, tools/area.py)
    # every corner quantised as collision.bin does (round((p - origin) / SCALE); numpy's rint rounds half to even
    # like round()), the triangles that collapse there skipped
    P = np.array([p for s in subs for t in s["tris"] for p in t], np.float64).reshape(-1, 3, 3)
    lo, hi = P.reshape(-1, 3).min(0).tolist(), P.reshape(-1, 3).max(0).tolist()
    origin = [round((lo[k] + hi[k]) / 2) for k in range(3)]
    Q = np.rint((P - np.array(origin, np.float64)) / export_collision.SCALE)
    ok = (Q[:, 0] != Q[:, 1]).any(1) & (Q[:, 1] != Q[:, 2]).any(1) & (Q[:, 0] != Q[:, 2]).any(1)
    nok = np.concatenate([[0], np.cumsum(ok)]).tolist()
    tri_key, count, i = [], {}, 0
    for s in subs:
        k = key_of(coll[s["part"]]) if s["part"] in coll else None
        n = nok[i + len(s["tris"])] - nok[i]
        i += len(s["tris"])
        if n:
            tri_key += [k] * n
            count[k] = count.get(k, 0) + n
    keys = [k for k in count if k is not None]
    if not keys:
        keys = [(main_fog, min(light_rows), min(tone_rows), min(tone_rows), -1, -1, -1)]
    main = max(keys, key=lambda k: (k[0] == main_fog, count.get(k, 0)))
    order = [main] + sorted((k for k in keys if k != main), key=lambda k: -count.get(k, 0))
    assert len(order) <= 255, "too many draw-param sets"
    tone, chars, pls, specs = {}, {}, {}, {}
    a = dsr_light.srgb_lin(mapcfg.CHAR_ALB)
    for k in order:
        f, l, t, c, lant, probe, _ = k
        row = light_rows.get(l, light_rows[min(light_rows)])
        tone[k] = area_tone(tone_rows, banks, t, c)
        if k != main:  # areas the map bake did not adapt to (cell, elevator): their own exposure
            tone[k] = adapted(tone[k], row, probes.get(probe))
        chars[k] = char_light(row, probes.get(probe), tone[k], shadow=k == main)
        pl = banks["PointLight"].get(lant)
        if pl and pl["colA"] > 0:  # the point light's share on top of the area light, for albedo CHAR_ALB, at full strength
            e0 = dsr_light.light_fn(row)((0, 1, 0), probes.get(probe))
            add = [dsr_light.lin(pl[f"col{ch}"], pl["colA"]) for ch in "RGB"]
            t0 = tone[k]
            kk = [(o1 - o0) / mapcfg.CHAR_ALB for o1, o0 in zip(t0([a * (e + x) for e, x in zip(e0, add)]), t0([a * e for e in e0]))]
            pls[k] = (min(255, round(10 * pl["dwindleBegin"])), [min(255, round(0x80 * x)) for x in kk],
                      min(255, round(10 * pl["dwindleEnd"])))
        else:
            pls[k] = (0, [0, 0, 0], 0)
        # specular for vu1skin (TECH_DEBT map-29): env reflection up / down = envSpc x the probe's GI_EnvSpc cube
        # (rough mip), the LightBank specular light *_s and its direction; tone mapped for albedo CHAR_ALB, 0x80 units
        env = [dsr_light.lin(row[f"envSpc_col{ch}"], row["envSpc_colA"]) for ch in "RGB"]
        up, down = dsr_light.env_hemispheres(spec_probes[probe]) if probe in spec_probes else ([0] * 3, [0] * 3)
        u8 = lambda hdr: [min(255, round(0x80 * o / mapcfg.CHAR_ALB)) for o in tone[k]([a * x for x in hdr])]
        specs[k] = (u8([e * x for e, x in zip(env, up)]), u8([e * x for e, x in zip(env, down)]),
                    u8([dsr_light.lin(row[f"col{ch}_s"], row["colA_s"]) for ch in "RGB"]),
                    [max(-127, min(127, round(127 * x))) for x in dsr_light.light_dir(row["degRotX_s"], row["degRotY_s"])])
    idx = {k: i for i, k in enumerate(order)}
    tri_row = bytes(idx.get(k, 0) for k in tri_key)
    out = struct.pack("<4I", len(order), len(tri_row), 0, 0)
    for k in order:
        v = fog_rows[k[0]]
        amb, sun, L = chars[k]
        pb, prgb, pe = pls[k]
        eu, ed, si, ls = specs[k]
        dof = banks["Dof"].get(k[6])
        dof = [min(255, max(0, round(x))) for x in (dof["farDofBegin"], dof["farDofEnd"], dof["farDofMul"])] if dof \
            else [0, 0, 0]
        out += struct.pack("<3f4B4B4B3bB4B3B3B3B3b3Bx", v["fogBeginZ"], v["fogEndZ"], v["degRotW"] / 100.0, *fog_rgb(v, tone[k]), 0,
                           *[min(255, round(0x80 * x)) for x in amb], 0, *[min(255, round(0x80 * x)) for x in sun], 0,
                           *[max(-127, min(127, round(127 * x))) for x in L], pb, *prgb, pe, *eu, *ed, *si, *ls, *dof)
    return out + tri_row, [(k, fog_rows[k[0]], chars[k] + (pls[k], specs[k])) for k in order], tone
