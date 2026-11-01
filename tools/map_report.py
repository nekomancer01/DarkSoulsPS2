#!/usr/bin/env python3
"""Per-part coverage report of a map export (area-burg-coverage): runs tools/export_map.py unchanged and writes
<out>.parts.json = {MSB part name: [triangles gathered, triangles after simplification]} plus the texture / chunk
section sizes, so missing pieces can be traced to the stage that dropped them (gather crop, dedup, simplify).
    tools/map_report.py <game> <out map.bin> <map_id>      (same env knobs as export_map.py)"""
import json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_map as E

stats = {}
_simplify = E.simplify_meshes


def simplify_meshes(meshes, *a, **kw):
    for m in meshes:
        stats.setdefault(m["name"].split("/")[0], [0, 0])[0] += len(m["tris"])
    r = _simplify(meshes, *a, **kw)
    for m in meshes:
        stats[m["name"].split("/")[0]][1] += len(m["tris"])
    return r


E.simplify_meshes = simplify_meshes
_write = E.texmap.write


def write(blob, hdr, textures, chunks):
    start = len(blob)
    r = _write(blob, hdr, textures, chunks)
    stats["#textures_bytes"] = [len(blob) - start, len(textures)]
    stats["#chunk_bytes"] = [start, len(chunks)]
    return r


E.texmap.write = write
E.main()
json.dump(stats, open(sys.argv[2] + ".parts.json", "w"), indent=0)
