#!/usr/bin/env python3
"""Place / read the reference DSR camera (duplicate install, tools/dsr-ref.sh). Runs on the host:
    distrobox-host-exec python3 -I tools/dsr-ref-cam.py [--pos X Y Z] [--yaw DEG] [--pitch DEG] [--hide] [--shot out.png]
    ... --point MAP N [--yaw DEG] [--pitch DEG] [--shot out.png]   stand on autotest point N of MAP (tools/dsr_points.py)
(tools/dsr-ref.sh cam ... runs it on the host.) The map must be the one loaded: warps stay inside the current map.

No args: print the current camera (pos, yaw, pitch, fov, aspect, near, far, the full 4x4 world matrix) as JSON.

DSR recomputes its follow camera every frame from the player position and its own yaw/pitch state, so direct writes
to the camera matrix do not stick (measured: overwritten within 5-30 ms). Placement is therefore closed-loop through
the game's own controls:
  - the player gets NoGravity (ChrFlags1 0x4000) and DisableMapHit (ChrMapFlags 0x10), so it floats where warped;
  - yaw / pitch are turned with relative mouse motion over libei (tools/dsr-ei), gain re-measured every step;
  - the player is warped by the remaining camera position error until the camera sits on --pos.
--hide turns off the Characters draw group (dsr_mem.py draw chr 0: hides every character, the player's SFX stay).
Camera fields (FollowCam, DSR 1.03.1): world matrix rows +0x10 right, +0x20 up, +0x30 forward, +0x40 position;
+0x50 fov (vertical, rad), aspect, near, far; +0x144 yaw (rad; forward = (sin yaw, ., cos yaw)); +0x180 distance.
Conventions: yaw = atan2(fwd.x, fwd.z), pitch = asin(fwd.y), both in DSR world space (the port's map space).
FOV stays the game's (LockCamParam-driven, 0.7505 rad at Firelink); writes to it are overwritten too.
"""
import argparse, glob, json, math, os, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsr_mem as d

EI = os.path.expanduser('~/dsr-ref/tools/dsr-ei')
NOGRAV, NOMAPHIT = 0x4000, 0x10


def ei_socket():
    rt = os.environ['XDG_RUNTIME_DIR']
    for s in sorted(glob.glob(rt + '/gamescope-*-ei')):
        base = os.path.basename(s)[:-3]
        out = subprocess.run(['gamescopectl'], env=dict(os.environ, GAMESCOPE_WAYLAND_DISPLAY=base),
                             capture_output=True, text=True)
        if 'Connector Name: Headless' in out.stdout + out.stderr:
            return s
    sys.exit('dsr-ref-cam: no headless gamescope')


def cam(m):
    c = d.follow_cam(m)
    mat = m.f32(c + 0x10, 16)
    fwd = mat[8:11]
    fov, aspect, near, far = m.f32(c + 0x50, 4)
    return {'pos': list(mat[12:15]), 'yaw': math.degrees(math.atan2(fwd[0], fwd[2])),
            'pitch': math.degrees(math.asin(max(-1, min(1, fwd[1])))), 'fov_deg': math.degrees(fov),
            'fov_rad': fov, 'aspect': aspect, 'near': near, 'far': far, 'dist': m.f32(c + 0x180),
            'matrix_rows': [list(mat[i:i + 4]) for i in range(0, 16, 4)]}


def flag(m, addr, bit, on):
    v = m.i32(addr) & 0xFFFFFFFF
    m.write(addr, ((v | bit) if on else (v & ~bit)).to_bytes(4, 'little'))


def hide_chr(m, hide):
    m.write(m.rel(d.GROUP_MASK, 2, 7) + d.GROUPS.index('chr'), bytes([0 if hide else 1]))


def stand(m, xyz):
    """Normal player on a ground point: gravity and map collision back on, warp, let it settle."""
    p = d.player(m)
    flag(m, p + 0x284 + 0x20, NOGRAV, False)
    flag(m, d.map_data(m) + 0x104, NOMAPHIT, False)
    d.warp(m, *xyz)
    time.sleep(1.5)


def screenshot(out):
    rt = os.environ['XDG_RUNTIME_DIR']
    base = os.path.basename(ei_socket())[:-3]
    tmp = os.path.expanduser('~/dsr-ref/shot.png')
    if os.path.exists(tmp):
        os.remove(tmp)
    subprocess.run(['gamescopectl', 'screenshot', tmp], env=dict(os.environ, GAMESCOPE_WAYLAND_DISPLAY=base),
                   capture_output=True)
    for _ in range(50):
        if os.path.exists(tmp) and os.path.getsize(tmp):
            break
        time.sleep(0.1)
    time.sleep(0.3)
    shutil.move(tmp, out)


def settle(m, timeout=4.0):
    """Wait until the camera stops moving (follow-cam smoothing; slow frames under RenderDoc)."""
    time.sleep(0.25)  # input -> next game frame (frames are ~100 ms apart under RenderDoc)
    t, last, still = time.time(), None, 0
    while time.time() - t < timeout:
        c = cam(m)
        cur = (round(c['yaw'], 3), round(c['pitch'], 3), *(round(v, 4) for v in c['pos']))
        still = still + 1 if cur == last else 0
        if still >= 8:  # unchanged for 0.4 s
            return
        last = cur
        time.sleep(0.05)


def wrap(a):
    return (a + 180) % 360 - 180


def place(m, pos, yaw, pitch, hide):
    p = d.player(m)
    flag(m, p + 0x284 + 0x20, NOGRAV, True)
    flag(m, d.map_data(m) + 0x104, NOMAPHIT, True)
    hide_chr(m, hide)
    sock = ei_socket()
    gain = {'x': 0.06, 'y': 0.06}  # deg per px (measured ~0.06 for yaw), refined from each move
    for it in range(40):
        c = cam(m)
        ey = wrap(yaw - c['yaw']) if yaw is not None else 0
        ep = (pitch - c['pitch']) if pitch is not None else 0
        e = [pos[i] - c['pos'][i] for i in range(3)] if pos else [0, 0, 0]
        en = math.sqrt(sum(v * v for v in e))
        if abs(ey) < 0.05 and abs(ep) < 0.05 and en < 0.01:
            return c, it
        if abs(ey) >= 0.05 or abs(ep) >= 0.05:
            dx = max(-1500, min(1500, ey / gain['x'])) if abs(ey) >= 0.05 else 0
            dy = max(-800, min(800, ep / gain['y'])) if abs(ep) >= 0.05 else 0
            # keep the libei connection open briefly: motion sent right before a disconnect is dropped
            subprocess.run([EI, sock, 'move', str(round(dx)), str(round(dy)), 'wait', '200'], check=True)
            if os.environ.get('CAM_DEBUG'):
                print('it %d yaw %.2f pitch %.2f move %d %d gain %s' % (it, c['yaw'], c['pitch'], dx, dy, gain), file=sys.stderr)
            settle(m)
            c2 = cam(m)
            for ax, dpx, before, after in (('x', dx, c['yaw'], c2['yaw']), ('y', dy, c['pitch'], c2['pitch'])):
                moved = wrap(after - before) if ax == 'x' else after - before
                if abs(round(dpx)) >= 5 and abs(moved) > 0.02:
                    gain[ax] = moved / round(dpx)  # also picks up an inverted axis
            continue
        x, y, z, _ = d.pos(m)
        d.warp(m, x + e[0], y + e[1], z + e[2])
        time.sleep(0.1)
        settle(m)
    return cam(m), -1


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--pos', type=float, nargs=3)
    ap.add_argument('--yaw', type=float)
    ap.add_argument('--pitch', type=float)
    ap.add_argument('--hide', action='store_true')
    ap.add_argument('--point', nargs=2, metavar=('MAP', 'N'))
    ap.add_argument('--shot')
    a = ap.parse_args()
    m = d.Mem()
    if a.point:
        import dsr_points
        f, k, xyz = dsr_points.load(a.point[0])[int(a.point[1])]
        stand(m, xyz)
    if a.pos or a.yaw is not None or a.pitch is not None:
        c, it = place(m, a.pos, a.yaw, a.pitch, a.hide)
        c['iterations'] = it
        if it < 0:
            print('dsr-ref-cam: did not converge', file=sys.stderr)
    else:
        c = cam(m)
    c['player'] = list(d.pos(m))
    if a.point:
        c['point'] = {'map': a.point[0], 'n': int(a.point[1]), 'file': f, 'index': k, 'xyz': list(xyz)}
    if a.shot:
        time.sleep(0.5)
        screenshot(os.path.abspath(a.shot))
        c['shot'] = os.path.abspath(a.shot)
    print(json.dumps(c, indent=1))
