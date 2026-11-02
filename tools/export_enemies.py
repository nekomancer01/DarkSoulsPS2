"""Enemy placements of a map (MSB enemy parts, type 2) for the PS2 prototype: enemies.bin in the area pack.

Layout (little-endian):
  header  "DSRN", u32 count
  entry   char model[8] ("c2900"), i32 npc param, i32 think param, i32 entity id, f32 x, y, z, yaw     (36 B)
yaw: radians in the game's convention (forward = (sin yaw, 0, cos yaw)) = MSB rot.y + pi, as the map's player spawn.
MSB enemy fields (think_id, npc_id, entity_id) come from souls.msb1. c0000 parts (human NPCs and phantoms) come after
the others, inside the area's region and not counted against the roster limit: the runtime places them as stand-ins
(ps2/proto/src/npc.c, TECH_DEBT enemy-14); older runtimes skip them (no c0000 chr.bin).
Usage: export_enemies.py <game dir> <out.bin> <map id>      (only the parts inside the area's region, tools/area.py)
       export_enemies.py --models <game dir> <map id>      (prints the area's enemy types)"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import area

# enemy_parts / npc_parts / MAX_ENEMIES live in area.py (per-area selection), so export_chr.py and lua50.py do not
# depend on this file
from area import MAX_ENEMIES, enemy_parts, npc_parts  # noqa: F401


def main():
    if sys.argv[1] == "--models":  # export_enemies.py --models <game dir> <map id>: the area's enemy types (Makefile)
        b = souls.read(f"{sys.argv[2]}/map/MapStudio/{sys.argv[3]}.msb")
        print(" ".join(sorted({r[0] for r in enemy_parts(b, sys.argv[3]) if r[0] != "c0000"})))
        return
    game, out, map_id = sys.argv[1], sys.argv[2], sys.argv[3]
    b = souls.read(f"{game}/map/MapStudio/{map_id}.msb")
    rows = [r for r in enemy_parts(b, map_id) if r[0] != "c0000"] + npc_parts(b, map_id)
    with open(out, "wb") as f:
        f.write(struct.pack("<4sI", b"DSRN", len(rows)))
        for model, npc, think, ent, pos, rot in rows:
            f.write(struct.pack("<8s3i4f", model.encode(), npc, think, ent, *pos, math.radians(rot[1]) + math.pi))
    models = sorted({r[0] for r in rows if r[0] != "c0000"})
    nnpc = sum(r[0] == "c0000" for r in rows)
    print(f"{map_id}: {len(rows) - nnpc} enemies + {nnpc} c0000 NPCs, models {models}")


if __name__ == "__main__":
    main()
