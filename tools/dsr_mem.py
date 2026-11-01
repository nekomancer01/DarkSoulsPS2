#!/usr/bin/env python3
"""Read / write the running reference DSR (duplicate install, tools/dsr-ref.sh) through /proc/<pid>/mem on the host.

Pointers: AOB patterns + offset chains from DSR-Gadget (DSROffsets.cs, DSR 1.03; GPL-3.0, facts only). The exe's
image sits at 0x140000000 under wine. Run on the host (distrobox-host-exec python3 ...); the game runs as our uid
and wine allows ptrace by any process (PR_SET_PTRACER_ANY), so no root is needed.

    dsr_mem.py pos                    player x y z angle
    dsr_mem.py ready                  exit 0 once a player is loaded in the world (title / loading: 1)
    dsr_mem.py warp X Y Z [ANGLE]     same-map teleport (ChrMapData warp request)
    dsr_mem.py bonfire [ID]           read / set the last bonfire (respawn point)
    dsr_mem.py hp [N]                 read / set HP (0 = die -> respawn at the last bonfire)
    dsr_mem.py cam                    camera object dump (floats) for layout work
    dsr_mem.py draw [GROUP 0|1 ...]   draw-group switches: map obj chr sfx cutscene (chr 0 hides every character)
    dsr_mem.py filter                 GraphicsData filter override + brightness/contrast/saturation/hue
"""
import glob, os, re, struct, sys

IMAGE = 0x140000000


def find_pid():
    for d in glob.glob('/proc/[0-9]*'):
        try:
            if open(d + '/cmdline', 'rb').read().split(b'\0')[0].endswith(b'DarkSoulsRemastered.exe') and \
               os.path.exists(d + '/maps') and 'dsr-ref' in open(d + '/maps').read(1 << 16):
                return int(d[6:])
        except OSError:
            pass
    sys.exit('dsr_mem: reference DSR is not running')


class Mem:
    def __init__(self, pid=None):
        self.pid = pid or find_pid()
        self.f = open(f'/proc/{self.pid}/mem', 'r+b', buffering=0)
        self._ptr = {}

    def read(self, a, n):
        self.f.seek(a)
        return self.f.read(n)

    def write(self, a, b):
        self.f.seek(a)
        self.f.write(b)

    def u64(self, a): return struct.unpack('<Q', self.read(a, 8))[0]
    def i32(self, a): return struct.unpack('<i', self.read(a, 4))[0]
    def f32(self, a, n=1):
        v = struct.unpack(f'<{n}f', self.read(a, 4 * n))
        return v if n > 1 else v[0]
    def w_i32(self, a, v): self.write(a, struct.pack('<i', v))
    def w_f32(self, a, *v): self.write(a, struct.pack(f'<{len(v)}f', *v))

    def image(self):
        """(start, bytes) of the exe's mapped image, unreadable gaps zero-filled."""
        if not hasattr(self, '_img'):
            size = struct.unpack('<I', self.read(IMAGE + self.i32(IMAGE + 0x3C) + 0x50, 4))[0]
            buf = bytearray(size)
            for line in open(f'/proc/{self.pid}/maps'):
                lo, hi = (int(x, 16) for x in line.split()[0].split('-'))
                lo, hi = max(lo, IMAGE), min(hi, IMAGE + size)
                if lo < hi:
                    try:
                        buf[lo - IMAGE:hi - IMAGE] = self.read(lo, hi - lo)
                    except OSError:
                        pass
            self._img = bytes(buf)
        return self._img

    def aob(self, pat):
        rx = b''.join(b'.' if t == '?' else re.escape(bytes([int(t, 16)])) for t in pat.split())
        m = re.search(rx, self.image(), re.S)
        if not m:
            sys.exit(f'dsr_mem: pattern not found: {pat}')
        return IMAGE + m.start()

    def rel(self, pat, off=3, size=7):
        """Address a rip-relative `mov reg, [rip+disp32]` at the pattern start points to (cached)."""
        if pat not in self._ptr:
            a = self.aob(pat)
            self._ptr[pat] = a + size + self.i32(a + off)
        return self._ptr[pat]

    def chain(self, base, *offs):
        """DSR-Gadget PHPointer: for each offset, addr = *(addr + offset)."""
        a = base
        for o in offs:
            a = self.u64(a + o)
        return a


# DSR-Gadget DSROffsets.cs (1.03: ChrClassWarpBoost 0x10, ChrData1Boost1 0x20, ChrData1Boost2 0x10)
WORLD_CHR = '48 8B 05 ? ? ? ? 48 8B 48 68 48 85 C9 0F 84 ? ? ? ? 48 39 5E 10 0F 84 ? ? ? ? 48'
CLASS_WARP = '48 8B 05 ? ? ? ? 66 0F 7F 80 ? ? ? ? 0F 28 02 66 0F 7F 80 ? ? ? ? C6 80'
FOLLOW_CAM = '48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B 4E 68 48 8B 05 ? ? ? ? 48 89 48 60'
CAM_MAN = '48 8B 05 ? ? ? ? 48 63 D1 48 8B 44 D0 08 C3'
GROUP_MASK = '80 3D ? ? ? ? 00 BE 00 00 00 80'  # cmp byte [rip+d32], 0: 5 bytes Map, Objects, Characters, SFX, Cutscenes
GROUPS = ('map', 'obj', 'chr', 'sfx', 'cutscene')
GRAPHICS = '48 8B 05 ? ? ? ? 48 8B 48 08 48 8B 01 48 8B 40 58'


def player(m):
    return m.chain(m.rel(WORLD_CHR), 0, 0x68)


def map_data(m):
    return m.u64(player(m) + 0x48 + 0x20)


def pos(m):
    p = m.u64(map_data(m) + 0x28)
    x, y, z = m.f32(p + 0x10, 3)
    return x, y, z, m.f32(p + 0x4)


def warp(m, x, y, z, ang=None):
    md = map_data(m)
    m.w_f32(md + 0x110, x, y, z)
    m.w_f32(md + 0x124, pos(m)[3] if ang is None else ang)
    m.write(md + 0x108, b'\1')


def class_warp(m):
    return m.chain(m.rel(CLASS_WARP), 0)


def follow_cam(m):
    return m.chain(m.rel(FOLLOW_CAM), 0, 0x60, 0x60)


def graphics(m):
    return m.chain(m.rel(GRAPHICS), 0, 0x738)


if __name__ == '__main__':
    m = Mem()
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'pos'
    a = [float(v) for v in sys.argv[2:]] if cmd != 'draw' else []
    if cmd == 'ready':
        try:
            x, y, z, _ = pos(m)
            sys.exit(0 if (x, y, z) != (0, 0, 0) else 1)
        except (OSError, struct.error):
            sys.exit(1)
    elif cmd == 'pos':
        print('pos %.3f %.3f %.3f angle %.4f' % pos(m))
    elif cmd == 'warp':
        warp(m, *a[:3], *(a[3:4] or [None]))
    elif cmd == 'bonfire':
        cw = class_warp(m) + 0xB24 + 0x10
        if a:
            m.w_i32(cw, int(a[0]))
        print('last bonfire', m.i32(cw))
    elif cmd == 'hp':
        hp = player(m) + 0x3D8 + 0x10
        if a:
            m.w_i32(hp, int(a[0]))
        print('hp', m.i32(hp), '/', m.i32(hp + 4))
    elif cmd == 'cam':
        c = follow_cam(m)
        print(hex(c))
        for o in range(0, 0x200, 16):
            print('%03x' % o, ' '.join('%10.4f' % v for v in m.f32(c + o, 4)))
    elif cmd == 'draw':
        g = m.rel(GROUP_MASK, 2, 7)
        args = sys.argv[2:]
        for name, v in zip(args[::2], args[1::2]):
            m.write(g + GROUPS.index(name), bytes([int(v)]))
        print(' '.join(f'{n}={b}' for n, b in zip(GROUPS, m.read(g, 5))))
    elif cmd == 'filter':
        g = graphics(m)
        print('override', m.read(g + 0x34D, 1)[0], 'bright', m.f32(g + 0x350, 3), 'sat', m.f32(g + 0x35C),
              'contrast', m.f32(g + 0x360, 3), 'hue', m.f32(g + 0x36C))
    else:
        sys.exit(__doc__)
