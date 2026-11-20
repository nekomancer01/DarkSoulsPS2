"""The VU1 memory contract (Polaris 1.5): where every program's data lives in VU1 data memory, where every program
sits in micro memory, what a chain must send before its first program call, and which registers carry over. One table
for gen.py (the programs), the EE (ps2/proto/src/vu1_layout.h, generated: `python3 gen.py layout`), the exporters that
write VIF packets (tools/mapchunks.py, tools/horizon.py, tools/mapcfg.py) and the VU1 emulator tests (through gen.py).

  python3 layout.py            the free-space report (data memory per chain kind, micro memory per bank)
  python3 layout.py --hash     the layout hash the exporters stamp into files that carry packets

Units: data memory in qwords (1024), micro memory in instruction pairs of 8 bytes (2048)."""
import hashlib
import os
import sys

DATA_QW = 1024
MICRO = 2048

# ---------------------------------------------------------------------------------------------- data memory (qwords)
# Every chain kind: the camera and fog constants at 0-7 (vu1_begin, vu1_map_fog): 0-3 view matrix columns (col3 =
# translation), 4 scale (F, -F, Zs, fog a), 5 offset (screen centre + GS window offset, 0, fog b), 6 clip scale
# (F/guard, F/guard, 1, fog min), 7 clip bias (0, 0, -2 near, fog max).
CONST, CONST_QW = 0, 8

# map pass (map chains: vu1map2, vu1mapi, vu1clip; VIF BASE MAP_IN, OFFSET MAP_IN_QW)
MAP_HDR = 7                         # header qwords of a list batch (vu1map2): 0 {tris, cull, st K, edge fog a2}, 1 origin..
CHUNK_TRIS = 54                     # triangles of a list batch: 7 + 9 x 54 = 493 qw, the buffer
MAP_IN, MAP_IN_QW = 8, MAP_HDR + 9 * CHUNK_TRIS  # input double buffer (BASE / OFFSET)
MAP_OUT0, MAP_OUT_QW = 994, 10      # two output slots of one triangle (GIF tag + 9 qw)
MAP_OUT1 = MAP_OUT0 + MAP_OUT_QW
MAP_PLANES = MAP_OUT1 + MAP_OUT_QW  # 5 clip planes, written by vu1_begin
MAP_PLANES_QW = 5
CLIP_TRIS = 32                      # clip batches (EE splits): scratch lives in the rest of the batch's own buffer
POLYA, POLYB, PV = 7 + 9 * CLIP_TRIS + 1, 7 + 9 * CLIP_TRIS + 1 + 27, 7 + 9 * CLIP_TRIS + 1 + 54
IDX_TRIS = 64         # vu1.c VU1_IDX_TRIS: triangles of an indexed batch (vu1clip's indexed entry lists all of them)
C2_SCR = 27 + 27 + 32 + 5 + 1 + IDX_TRIS + 1  # scratch at the end of the batch's own buffer
C2_POLYA = MAP_IN_QW - C2_SCR
C2_POLYB, C2_PV = C2_POLYA + 27, C2_POLYA + 54
NEED = C2_PV + 32     # clip_core: one word per clip plane
MODE = NEED + 5       # pass 2: 0 list batch, 1 indexed batch
LIST = MODE + 1       # pass 1: TOPS-relative address of each flagged triangle (indexed: its triangle stream qword), 0-ended
assert 7 + 9 * CLIP_TRIS <= C2_POLYA and LIST + IDX_TRIS + 1 <= MAP_IN_QW
CLIPI_MAX = C2_POLYA  # indexed batches through the indexed entry: header + vertices + triangle stream <= this
MAPI_HDR = 8  # header qwords of an indexed batch: vu1map2's 7 + {vertex passes, triangle stream offset, 0, 0}
# Map specular constants (vu1mapspec, Polaris 2.3, written by vu1_map_spec before the pass; world space): 0 Ec = (EnvUp +
# EnvDown) / 2 with .w = 128 (the vertex alpha), 1 Eh = (EnvUp - EnvDown) / 2, 2 SpecI (colours in 0x80 units, .w 0),
# 3 (255, 255, 255, 1 / 127) the colour clamp and the scale of the vertices' u8 F0. The batch header
# carries the rest (per chunk: the dot rows and the Blinn fit; vu1mapspec.vsm's doc). In the 5 qwords after the clip
# planes (render-21: 1 left free)
MAP_SPEC = MAP_PLANES + MAP_PLANES_QW
MAP_SPEC_QW = 4

# skin pass (character chains: vu1skin2, vu1skinclip; VIF BASE SKIN_IN, OFFSET SKIN_IN_SIZE)
BONE_WIN, BONE_QW, BONE_MAX = 8, 4, 61  # the bone palette window: 4 qwords a bone, 61 bones (vu1_skin_window pages bigger skeletons through it)
SKIN_KICK = 8
SKIN_TRIS = 24
SKIN_IN = BONE_WIN + BONE_QW * BONE_MAX  # 252
SKIN_IN_SIZE = 9 + 12 * SKIN_TRIS
SKIN_OUT0 = SKIN_IN + 2 * SKIN_IN_SIZE
SKIN_OUT_SIZE = 1 + 9 * SKIN_KICK
SKIN_OUT1 = SKIN_OUT0 + SKIN_OUT_SIZE
# Specular constant block (skin pass, written per draw by vu1_skin_begin; view space): 0-2 Mn_k = (Ls_k, 2 Up_k,
# Vc_k, Hc_k) with Vc = towards the camera from the character's centre, Hc = normalize(Ls + Vc); 3 (BIG, -Vc.Up, 0, 0);
# 4 Ec = (EnvUp + EnvDown) / 2, 5 Eh = (EnvUp - EnvDown) / 2, 6 SpecI, 7 amb, 8 sun (colours x 1/255: m and f arrive as
# 0..255, colour lanes .w = 0), 9 Eh / 2. Batch header 0.zw = (a, b): d = max(0, a n.H + b)^2 ~ (n.H)^(2^k) (vu1.c).
SPEC = 992
SPEC_QW = 10  # 9: Eh / 2 (vu1skinclip's exact path)
SKIN_CLIP_TRIS = 12
SPOLYA = 9 + 12 * SKIN_CLIP_TRIS + 1
SPOLYB, SPV = SPOLYA + 27, SPOLYA + 54

# particles (effect chains: vu1fx; VIF BASE FX_IN, OFFSET FX_IN_QW)
FX_SC = 8                            # 5 qw: sin / cos coefficients in .xy, motion series phi2 / phi1 in .zw (vu1_fx_begin)
FX_SC_QW = 5
FX_MK = FX_SC + FX_SC_QW             # 2 qw motion constants (gen.py fx_motion_const) (vu1_fx_begin)
FX_MK_QW = 2
FX_DBL = 5                           # motion: drag x age halved FX_DBL times for the series, then doubled back
FX_XMAX = 0.5 * 2 ** FX_DBL          # largest drag x life the series covers (|error| < 1e-4); fx.c sends others pre-moved
FX_IN = 16                           # input double buffer (BASE / OFFSET)
FX_N = 56                            # particles per batch (even: the loop takes FX_P at a time)
FX_SYS = 8                           # curve tables per batch
FX_HDR = 17                          # batch header qwords: 0-14 as before, 15 / 16 the quad axes of oriented batches
FX_TAB = FX_HDR                      # batch offset of the curve tables (FX_SYS x 16 qw; size keys' .zw = drag, gravity)
FX_PQW = 3                           # qwords per particle: {spawn xyz, key}, {half size x y, roll, frame}, {velocity, age}
FX_PART = FX_TAB + 16 * FX_SYS       # batch offset of the particles
FX_IN_QW = FX_PART + FX_PQW * FX_N
FX_KICK = 14                         # particles per output kick
FX_OUT0 = FX_IN + 2 * FX_IN_QW
FX_OUT_QW = 1 + 12 * FX_KICK
FX_OUT1 = FX_OUT0 + FX_OUT_QW
FX_P = 2                             # particles per loop pass

# Regions per chain kind (name, base, qwords), for the overlap check and the free-space report. A program may also use
# the rest of its own input buffer as scratch (the clip programs: POLYA.., C2_*, SPOLYA..): inside the buffer rows.
REGIONS = {
    "map": [("constants", CONST, CONST_QW), ("input x2", MAP_IN, 2 * MAP_IN_QW), ("output x2", MAP_OUT0, 2 * MAP_OUT_QW),
            ("clip planes", MAP_PLANES, MAP_PLANES_QW), ("map specular", MAP_SPEC, MAP_SPEC_QW)],
    "skin": [("constants", CONST, CONST_QW), ("bone window", BONE_WIN, BONE_QW * BONE_MAX),
             ("input x2", SKIN_IN, 2 * SKIN_IN_SIZE), ("output x2", SKIN_OUT0, 2 * SKIN_OUT_SIZE),
             ("specular", SPEC, SPEC_QW), ("clip planes", MAP_PLANES, MAP_PLANES_QW)],
    "fx": [("constants", CONST, CONST_QW), ("sin / cos", FX_SC, FX_SC_QW), ("motion", FX_MK, FX_MK_QW), ("input x2", FX_IN, 2 * FX_IN_QW),
           ("output x2", FX_OUT0, 2 * FX_OUT_QW)],
}
# VIF double buffering per chain kind: BASE, OFFSET (vu1.c sets them in each chain's preamble); programs read their
# input through the TOP register (xtop), which alternates BASE / BASE + OFFSET per MSCAL
VIF = {"map": (MAP_IN, MAP_IN_QW), "skin": (SKIN_IN, SKIN_IN_SIZE), "fx": (FX_IN, FX_IN_QW)}
# The VU1 chain buffer each kind is built in (vu1_begin's numbers; src/ot.c maps buckets onto them)
CHAIN_BUF = {"map": 0, "skin": 1, "fx": 2}
# What a chain sends before its first program call (vu1_begin and the kind's begin call). The order table's segments
# that inherit a buffer (ot.h rule 2) assume the previous segment of the same kind left exactly this in place.
PREAMBLE = {
    "map": "FLUSHE; STCYCL 1,1; BASE / OFFSET (VIF above); camera + fog constants 0-7; clip planes; the map programs "
           "loaded on demand (MPG, R8)",
    "skin": "as map, then vu1_skin_begin: BASE / OFFSET for the skin pass, the bone window, the light / specular block "
            "when used; the skin programs loaded on demand",
    "fx": "as map, then vu1_map_fog and vu1_fx_begin: BASE / OFFSET for particles, the sin / cos and motion blocks, the "
          "VIF MASK 0x50505050 of the masked curve-table unpacks (per system ROW from its table; vu1_fx_add restores ROW to 0 "
          "after the batch; the map and character masked unpacks set their own MASK 0xF0 first, which keeps .zw and never "
          "reads ROW); vu1fx loaded",
}
# VU1 registers a program expects to keep their value from an earlier call in the same chain. None today: every
# program reloads its constants from data memory at its entry and keeps nothing in VF / VI registers between MSCALs.
CARRIED_REGISTERS = ()

# ---------------------------------------------------------------------------------------------- micro memory
# One row per program variant: name (vu1.c enum P_<name>), code symbol prefix, address, chain bank, its .vsm.
# Programs of one bank are resident together; another bank's program over them costs one reload (R8: on demand).
_PROGRAMS = (
    ("MAP2", "VU1Map2", 1608, "map", "vu1map2.vsm"),
    ("CLIP", "VU1Clip", 0, "map", "vu1clip.vsm"),
    ("SKIN2", "VU1Skin2", 1024, "chr", "vu1skin2.vsm"),
    ("SKINCLIP", "VU1SkinClip", 384, "chr", "vu1skinclip.vsm"),
    ("FX", "VU1Fx", None, "fx", "vu1fx.vsm"),             # placed by place_fx() below (Polaris 2.6)
    ("MAPI", "VU1MapI", 580, "map", "vu1mapi.vsm"),
    ("MAPSPEC", "VU1MapSpec", 840, "map", "vu1mapspec.vsm"),  # the specular pass (Polaris 2.3): after MAPI's range
)
ALTERNATES = set()  # pairs never resident together, allowed to overlap (none since the VU1_MAP_OLD program left, render-19 PR)

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ps2", "proto", "src")


# Map programs the effect program may lie over when no gap between the map programs fits it: the specular pass
# (Polaris 2.3, vu1mapspec) runs once per frame in the map chain, so covering it costs one reload a frame (R8)
FX_MAY_COVER = ("MAPSPEC",)


def place_fx(programs):
    """the effect program's address, from the other programs' sizes (no fixed address assumes another program's size):
    end-aligned in the largest gap the map bank leaves (the map chain starts every frame, so the effect program never
    costs a map program reload); it may cover character programs, which reload on demand (R8) after an effect chain.
    When no gap fits (the map bank with the specular pass leaves 556 pairs), the highest address that covers only
    FX_MAY_COVER map programs. covers(): what it lies over"""
    size = program_size([p for p in programs if p[0] == "FX"][0][4])
    spans = sorted((p[2], p[2] + program_size(p[4]), p[0]) for p in programs if p[3] == "map" and p[0] != "MAP")
    gaps, at = [], 0
    for a, b, _ in spans + [(MICRO, MICRO, None)]:
        if a - at >= size:
            gaps.append((a - at, at, a))
        at = max(at, b)
    if gaps:
        _, lo, hi = max(gaps)
        return (hi - size) & ~1
    for a in range((MICRO - size) & ~1, -1, -2):
        if all(n in FX_MAY_COVER for lo, hi, n in spans if lo < a + size and a < hi):
            return a
    raise ValueError("layout: no room of %d pairs for FX over the map programs it may cover %s" % (size, FX_MAY_COVER))


def program_size(vsm):
    """instruction pairs between <sym>_CodeStart and _CodeEnd, rounded up to the .align 4 (2 pairs)"""
    n, inside = 0, False
    for line in open(os.path.join(SRC, vsm)):
        s = line.split(";")[0].strip()
        if not s:
            continue
        if s.endswith("CodeStart:"):
            inside = True
        elif s.endswith("CodeEnd:"):
            inside = False
        elif inside and not s.endswith(":") and not s.startswith("."):
            n += 1
    return n + (n & 1)


PROGRAMS = tuple((p[0], p[1], place_fx(_PROGRAMS) if p[2] is None else p[2]) + p[3:] for p in _PROGRAMS)


def covers(name="FX"):
    """the other banks' programs a program lies over (each costs a reload when chains alternate): [(name, lo, hi)]"""
    me = [p for p in PROGRAMS if p[0] == name][0]
    a, b = me[2], me[2] + program_size(me[4])
    return [(p[0], max(a, p[2]), min(b, p[2] + program_size(p[4])) - 1) for p in PROGRAMS
            if p[3] != me[3] and p[2] < b and a < p[2] + program_size(p[4])]


def check():
    """the contract's own consistency: regions inside data memory and not overlapping per kind, programs inside micro
    memory and not overlapping within a bank. Returns the problems"""
    bad = []
    for kind, rows in REGIONS.items():
        spans = sorted((b, b + n, name) for name, b, n in rows)
        for a, b in zip(spans, spans[1:]):
            if a[1] > b[0]:
                bad.append(f"data {kind}: {a[2]} {a[0]}..{a[1]} overlaps {b[2]} {b[0]}..{b[1]}")
        if spans[-1][1] > DATA_QW:
            bad.append(f"data {kind}: {spans[-1][2]} ends at {spans[-1][1]} > {DATA_QW}")
    progs = [(p[2], p[2] + program_size(p[4]), p[0], p[3]) for p in PROGRAMS]
    for a in progs:
        if a[1] > MICRO:
            bad.append(f"micro: {a[2]} ends at {a[1]} > {MICRO}")
        for b in progs:
            if a[2] < b[2] and a[3] == b[3] and a[0] < b[1] and b[0] < a[1] and (a[2], b[2]) not in ALTERNATES \
                    and (b[2], a[2]) not in ALTERNATES:
                bad.append(f"micro {a[3]}: {a[2]} {a[0]}..{a[1]} overlaps {b[2]} {b[0]}..{b[1]}")
    return bad


def layout_hash():
    """hash of everything an exported packet depends on (batch headers, buffer sizes, stream destinations)"""
    key = repr((MAP_HDR, CHUNK_TRIS, MAP_IN_QW, MAPI_HDR, CLIPI_MAX, IDX_TRIS, SKIN_IN_SIZE, FX_IN_QW)).encode()
    return int.from_bytes(hashlib.sha1(key).digest()[:4], "little")


def report():
    out = []
    for kind, rows in REGIONS.items():
        used = sum(n for _, _, n in rows)
        out.append(f"data {kind:5s}: {used:4d} of {DATA_QW} qw used, {DATA_QW - used:4d} free  ("
                   + ", ".join(f"{name} {b}..{b + n - 1}" for name, b, n in sorted(rows, key=lambda r: r[1])) + ")")
    sizes = {p[0]: program_size(p[4]) for p in PROGRAMS}
    taken = set()
    for p in PROGRAMS:
        taken |= set(range(p[2], p[2] + sizes[p[0]]))
    for bank in ("map", "chr", "fx"):
        ps = [p for p in PROGRAMS if p[3] == bank]
        used = sum(sizes[p[0]] for p in ps)
        out.append(f"micro {bank:4s}: {used:4d} of {MICRO} used by " + ", ".join(
            f"{p[0]} {p[2]}..{p[2] + sizes[p[0]] - 1}" for p in ps) + f"; {MICRO - used} free if only this bank is resident")
    out.append("micro FX covers: " + (", ".join(f"{n} {a}..{b}" for n, a, b in covers()) or "nothing"))
    out.append(f"micro all : {len(taken)} of {MICRO} covered by some program, {MICRO - len(taken)} never used")
    return "\n".join(out)


if __name__ == "__main__":
    problems = check()
    if "--hash" in sys.argv:
        print("0x%08x" % layout_hash())
    else:
        print(report())
    for p in problems:
        print("layout: " + p, file=sys.stderr)
    sys.exit(1 if problems else 0)
