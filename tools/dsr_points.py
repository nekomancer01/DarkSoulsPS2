#!/usr/bin/env python3
"""FromSoft autotest points: map/autotest/<map>_pNN.pointlist (a `!msb offset` header, then one x,y,z per line).

    dsr_points.py                     list every file and its point count
    dsr_points.py m10_02_00_00 [N]    points of one map (all lists, numbered in file order), or point N only
"""
import glob, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gamepath import game_dir
GAME = game_dir()  # autotest files are identical in the duplicate install


def load(map_id=None):
    """[(file, index_in_file, (x, y, z))] for every pointlist (of map_id), files in name order."""
    out = []
    for f in sorted(glob.glob(f'{GAME}/map/autotest/{map_id or "*"}_p*.pointlist')):
        n = 0
        for line in open(f, encoding='latin-1'):
            line = line.strip()
            if not line or line[0] in '!#':  # header / Shift-JIS comment lines
                continue
            out.append((os.path.basename(f), n, tuple(float(v) for v in line.split(','))))
            n += 1
    return out


if __name__ == '__main__':
    if len(sys.argv) < 2:
        files = {}
        for f, _, _ in load():
            files[f] = files.get(f, 0) + 1
        for f, n in files.items():
            print(f'{f:30} {n:4}')
        sys.exit()
    pts = load(sys.argv[1])
    sel = [int(sys.argv[2])] if len(sys.argv) > 2 else range(len(pts))
    for i in sel:
        f, k, (x, y, z) = pts[i]
        print(f'{i:4} {f:30} {k:4} {x:10.3f} {y:10.3f} {z:10.3f}')
