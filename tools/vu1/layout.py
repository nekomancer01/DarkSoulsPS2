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

# skin pass (character chains: vu1skin2, vu1skinclip; VIF BASE SKIN_IN, OFFSET SKIN_IN_SIZE)
# The bone palette window: 4 qwords a bone, 40 bones; model.c pages every skeleton over 40 through it by bone windows
# (Polaris 2.2 PR 2; was 61, windows only above it). DSR's meshes use <= 38 bones each (FLVER mesh bone tables, all 171
# chr/ FLVERs), so a window still spans whole meshes; the 21 slots freed pay for 27-triangle batches (were 24).
# VU1_BONE_MAX=61 in the environment (make VU1_BONES=61) builds the old layout: one 61-bone window, 23-triangle batches,
# windows only for skeletons over 61. The Makefile then generates this header and the skin programs into the build dir.
BONE_WIN, BONE_QW, BONE_MAX = 8, 4, int(os.environ.get("VU1_BONE_MAX") or 40)
SKIN_KICK = 8
SKIN_IN = BONE_WIN + BONE_QW * BONE_MAX  # 168 (61 bones: 252)
SKIN_OUT_SIZE = 1 + 9 * SKIN_KICK
SKIN_TOP = 980  # LIGHT (Polaris 2.2 PR 3; was SPEC 992): the skin pass's input double buffer and two output slots end below it
# the most triangles a batch can take: 2 input buffers of 9 + 12 per triangle and 2 output slots between SKIN_IN and SKIN_TOP
SKIN_TRIS = ((SKIN_TOP - SKIN_IN - 2 * SKIN_OUT_SIZE) // 2 - 9) // 12  # 27 (61 bones: 23)
SKIN_IN_SIZE = 9 + 12 * SKIN_TRIS
SKIN_OUT0 = SKIN_IN + 2 * SKIN_IN_SIZE
SKIN_OUT1 = SKIN_OUT0 + SKIN_OUT_SIZE
# Specular constant block (skin pass, written per draw by vu1_skin_begin; view space): 0-2 Mn_k = (Ls_k, 2 Up_k,
# Vc_k, Hc_k) with Vc = towards the camera from the character's centre, Hc = normalize(Ls + Vc); 3 (BIG, -Vc.Up, 0, 0);
# 4 Ec = (EnvUp + EnvDown) / 2, 5 Eh = (EnvUp - EnvDown) / 2, 6 SpecI, 7 amb, 8 sun (colours x 1/255: m and f arrive as
# 0..255, colour lanes .w = 0), 9 Eh / 2. Batch header 0.zw = (a, b): d = max(0, a n.H + b)^2 ~ (n.H)^(2^k) (vu1.c).
SPEC = 992
SPEC_QW = 10  # 9: Eh / 2 (vu1skinclip's exact path)
# Per-draw skin terms (Polaris 2.2 prebuilt packets: a model's vu1skin2 batch headers are built once and called, so what
# changes per draw lives here, written by vu1_skin_begin; each vu1skin2 entry adds them to its batch header in its
# prologue): 0 K1 add of textured batches (tint + amb, .w 0), 1 K2 add of textured batches (sun, .w 0), 2 K1 add of
# flat batches (tint, .w 0), 3 / 4 the PRIM GIF tag template of textured / flat batches (FGE when the draw fogs)
SKIN_DRAW = SPEC + SPEC_QW
SKIN_DRAW_QW = 5
# Character lights (Polaris 2.2 PR 3, written per draw by vu1_skin_begin; view space): 0-2 the light direction matrix's
# columns M_k = (Lsun_k, Lp1_k, Lp2_k, k == 0 ? 1/255 : 0) (towards each light: the area's directional light and the two
# point lights chrlight.c picked, from the character's centre), 3-5 their colours (0x80 = 1.0, .w 0). Textured
# batches: diffuse D = sum_k colour_k max(n . L_k, 0) on the view-space normal (bone A rotation)
LIGHT = 980
LIGHT_QW = 6
SKIN_CLIP_TRIS = 12
SPOLYA = 9 + 12 * SKIN_CLIP_TRIS + 1
SPOLYB, SPV = SPOLYA + 27, SPOLYA + 54

# particles (effect chains: vu1fx; VIF BASE FX_IN, OFFSET FX_IN_QW)
FX_SC = 8                            # 5 qw sin / cos coefficients (vu1_fx_begin)
FX_SC_QW = 5
FX_IN = 16                           # input double buffer (BASE / OFFSET)
FX_N = 64                            # particles per batch (even: the loop takes FX_P at a time)
FX_SYS = 8                           # curve tables per batch
FX_TAB = 15                          # batch offset of the curve tables (FX_SYS x 16 qw)
FX_PART = FX_TAB + 16 * FX_SYS       # batch offset of the particles (2 qw each)
FX_IN_QW = FX_PART + 2 * FX_N
FX_KICK = 16                         # particles per output kick
FX_OUT0 = FX_IN + 2 * FX_IN_QW
FX_OUT_QW = 1 + 12 * FX_KICK
FX_OUT1 = FX_OUT0 + FX_OUT_QW
FX_P = 2                             # particles per loop pass

# Regions per chain kind (name, base, qwords), for the overlap check and the free-space report. A program may also use
# the rest of its own input buffer as scratch (the clip programs: POLYA.., C2_*, SPOLYA..): inside the buffer rows.
REGIONS = {
    "map": [("constants", CONST, CONST_QW), ("input x2", MAP_IN, 2 * MAP_IN_QW), ("output x2", MAP_OUT0, 2 * MAP_OUT_QW),
            ("clip planes", MAP_PLANES, MAP_PLANES_QW)],
    "skin": [("constants", CONST, CONST_QW), ("bone window", BONE_WIN, BONE_QW * BONE_MAX),
             ("input x2", SKIN_IN, 2 * SKIN_IN_SIZE), ("output x2", SKIN_OUT0, 2 * SKIN_OUT_SIZE),
             ("specular", SPEC, SPEC_QW), ("per draw", SKIN_DRAW, SKIN_DRAW_QW), ("lights", LIGHT, LIGHT_QW), ("clip planes", MAP_PLANES, MAP_PLANES_QW)],
    "fx": [("constants", CONST, CONST_QW), ("sin / cos", FX_SC, FX_SC_QW), ("input x2", FX_IN, 2 * FX_IN_QW),
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
    "skin": "as map, then vu1_skin_begin: BASE / OFFSET for the skin pass, the bone window, the light / specular block, "
            "the per-draw block (SKIN_DRAW); the skin programs loaded on demand",
    "fx": "as map, then vu1_map_fog and vu1_fx_begin: BASE / OFFSET for particles, the sin / cos block; vu1fx loaded",
}
# VU1 registers a program expects to keep their value from an earlier call in the same chain. None today: every
# program reloads its constants from data memory at its entry and keeps nothing in VF / VI registers between MSCALs.
CARRIED_REGISTERS = ()

# ---------------------------------------------------------------------------------------------- micro memory
# One row per program variant: name (vu1.c enum P_<name>), code symbol prefix, address, chain bank, its .vsm.
# Programs of one bank are resident together; another bank's program over them costs one reload (R8: on demand).
PROGRAMS = (
    ("MAP", "VU1Map", 0, "map", "vu1map.vsm"),            # hand-written A/B program (VU1_MAP_OLD)
    ("MAP2", "VU1Map2", 1608, "map", "vu1map2.vsm"),
    ("CLIP", "VU1Clip", 0, "map", "vu1clip.vsm"),
    ("SKIN2", "VU1Skin2", 768, "chr", "vu1skin2.vsm"),          # (Polaris 2.2 PR 3: the light terms grew both skin
    ("SKINCLIP", "VU1SkinClip", 0, "chr", "vu1skinclip.vsm"),    # programs; was 1024 / 384. Away from FX at the top)
    ("FX", "VU1Fx", 1812, "fx", "vu1fx.vsm"),
    ("MAPI", "VU1MapI", 580, "map", "vu1mapi.vsm"),
)
ALTERNATES = {("MAP", "CLIP")}  # never resident together (VU1_MAP_OLD builds only): allowed to overlap

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ps2", "proto", "src")


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
    # (no skin term: no exporter writes skin packets, model.c builds them at load, so VU1_BONE_MAX=61 reads the same map.bin)
    key = repr((MAP_HDR, CHUNK_TRIS, MAP_IN_QW, MAPI_HDR, CLIPI_MAX, IDX_TRIS, FX_IN_QW)).encode()
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
        ps = [p for p in PROGRAMS if p[3] == bank and not (bank == "map" and p[0] == "MAP")]
        used = sum(sizes[p[0]] for p in ps)
        out.append(f"micro {bank:4s}: {used:4d} of {MICRO} used by " + ", ".join(
            f"{p[0]} {p[2]}..{p[2] + sizes[p[0]] - 1}" for p in ps) + f"; {MICRO - used} free if only this bank is resident")
    out.append(f"micro all : {len(taken)} of {MICRO} covered by some program, {MICRO - len(taken)} never used "
               f"(the A/B MAP {sizes['MAP']} included)")
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
