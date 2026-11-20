"""What the port runs, read from its own sources, so the coverage scan (tools/scan/scan.py) can mark each variant a
reader knows as ported or not. Nothing here reads the game.

  lua_ops()        Lua 5.0 opcode names lua0.c executes (its `case OP_*:` labels)
  esd_handled()    ESD env function ids and command ids per character kind (the case labels of player.c / enemy.c /
                   esd_env_move.c handlers; commands are matched by id, the handlers switch on the id alone)
  emevd_class()    (bank, id) -> export_emevd.OPS class: real / noop / stub (missing = unsupported)
  ffx_data()       FFX action data subtypes export_effects.py turns into particle systems (appearance, emitter, motion)
  tae_types()      TAE event types an exporter reads (the list below, with where)
"""
import functools
import os
import re
import sys

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(TOOLS, "..", "ps2", "proto", "src")
sys.path.insert(0, TOOLS)


def _read(name):
    with open(os.path.join(SRC, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def _body(src, func):
    """The {...} body of C function func (its definition, not a prototype)."""
    m = re.search(r"^[\w \t\*]*\b" + re.escape(func) + r"\s*\([^;{]*\)\s*\{", src, re.M)
    if not m:
        raise KeyError(func)
    i, depth = m.end() - 1, 0
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[i:j + 1]
    return src[i:]


@functools.lru_cache(None)
def _consts():
    """Integer #defines and enum members of every header / source file in ps2/proto/src (name -> value)."""
    out = {}
    for f in sorted(os.listdir(SRC)):
        if not f.endswith((".h", ".c")):
            continue
        s = _read(f)
        for name, val in re.findall(r"^#define\s+(\w+)\s+\(?(-?(?:0x[0-9a-fA-F]+|\d+))\)?\s*(?:/[*/].*)?$", s, re.M):
            out.setdefault(name, int(val, 0))
        for body in re.findall(r"enum\s*\w*\s*\{([^}]*)\}", s):
            body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)
            v = -1
            for item in body.split(","):
                item = item.strip()
                if not item:
                    continue
                m = re.match(r"(\w+)\s*(?:=\s*(.+))?$", item)
                if not m:
                    continue
                if m.group(2) is not None:
                    try:
                        v = int(m.group(2).strip(), 0)
                    except ValueError:
                        v = out.get(m.group(2).strip(), v + 1)
                else:
                    v += 1
                out.setdefault(m.group(1), v)
    return out


def _cases(body):
    """Integer values of the case labels in a C function body (symbolic ones resolved through _consts)."""
    out = set()
    for lab in re.findall(r"\bcase\s+([^:]+?)\s*:", re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)):
        try:
            out.add(int(lab, 0))
        except ValueError:
            v = _consts().get(lab.strip("() "))
            if v is not None:
                out.add(v)
    return out


@functools.lru_cache(None)
def lua_ops():
    return frozenset(re.findall(r"\bcase\s+OP_(\w+)\s*:", _read("lua0.c")))


# ESD handlers: (file, env function, command function) per kind; esd_env_move / esd_cmd_move are the shared movement
# set both kinds call first (esd_env_move.c)
ESD_HANDLERS = {"player": ("player.c", "player_esd_env", "player_esd_cmd"),
                "enemy": ("enemy.c", "enemy_env", "enemy_cmd")}


@functools.lru_cache(None)
def esd_handled():
    """{kind: (env ids, command ids)} for kind in player / enemy."""
    move = _read("esd_env_move.c")
    menv, mcmd = _cases(_body(move, "esd_env_move")), _cases(_body(move, "esd_cmd_move"))
    out = {}
    for kind, (f, env, cmd) in ESD_HANDLERS.items():
        s = _read(f)
        out[kind] = (frozenset(_cases(_body(s, env)) | menv), frozenset(_cases(_body(s, cmd)) | mcmd))
    return out


@functools.lru_cache(None)
def emevd_class():
    import export_emevd
    return {k: c for _, k, c in export_emevd.OPS if k}


@functools.lru_cache(None)
def ffx_data():
    import export_effects
    return frozenset(export_effects.APPEAR | {108} | set(export_effects.EMIT) | export_effects.MOVE)


# TAE event types the exporters read (DSAnimStudio TAE.Template.DS1.xml names)
TAE_TYPES = {
    0: "ChrActionFlag (export_player.py, export_chr.py)",
    1: "AttackBehavior (export_player.py, export_chr.py)",
    64: "CastHighlightedMagic (export_player.py)",
    118: "SpawnFFX_Blade (export_combat.py TAE_BLADE)",
    128: "PlaySound_CenterBody (export_audio.py)",
    129: "PlaySound_ByStateInfo (export_audio.py)",
    130: "PlaySound_ByDummyPoly (export_audio.py)",
    225: "SetSPRegenRatePercent (export_player.py)",
    231: "EzState message (export_player.py, export_chr.py)",
    236: "RootMotionReduction (export_player.py)",
    300: "SetTurnSpeedGradient / gravity (export_player.py)",
    304: "Throw (export_combat.py)",
}


def tae_types():
    return frozenset(TAE_TYPES)


# Havok classes a reader decodes (the object classes it looks up; the rest of a file's classes are their members)
HAVOK_READ = {
    "hkaSkeleton": "havok_anim.skeleton", "hkaSplineCompressedAnimation": "havok_anim.animation",
    "hkaAnimationBinding": "havok_anim.animation", "hkaRagdollInstance": "havok_ragdoll.ragdoll",
    "hkpStorageExtendedMeshShape": "havok_collision.hkx_meshes",
    "CustomParamStorageExtendedMeshShape": "havok_collision.hkx_meshes",
}


if __name__ == "__main__":
    print("lua ops", len(lua_ops()), sorted(lua_ops()))
    for k, (e, c) in esd_handled().items():
        print("esd", k, "env", len(e), "cmd", len(c))
    print("emevd ops", len(emevd_class()))
    print("ffx data", sorted(ffx_data()))
