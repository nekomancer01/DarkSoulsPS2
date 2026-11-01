"""FEV1 reader (FMOD Designer 4.x event project, as shipped in DSR sound/*.fev; version 0x003D0000).
Reverse-engineered from the game's files (no public spec); every file in sound/ must parse to its last byte before the
trailing chunks, which is the check `fev.py --all` runs.

Layout (little-endian, str = u32 length incl. NUL + bytes):
  "FEV1" u32 version, u32 ?, u32 ?, str project
  u32 nbanks, nbanks * {u32 load flags, u32 max streams, u8[8] hash, str bank name}
  category tree: {str name, f32 volume, f32 pitch, u32 ?, u32 ?, u32 nchildren, children...}
  u32 ngroups, groups: {str name, u32 nuserprops, userprops, u32 nsubgroups, subgroups..., u32 nevents, events...}
  event (type 8 = complex, 16 = simple; a simple event has u32 nsounds + sounds where a complex one has its layers,
         and no parameters or user properties):
    str name, guid[16], f32 volume, f32 pitch, f32 pitch randomisation, f32 volume randomisation, u32 priority,
    u32 max playbacks, u32 steal priority, u32 mode (FMOD_3D 0x10, custom rolloff 0x04000000, ...),
    f32 x 13 3D block (min distance, max distance, ...), ... (EV_FIXED below), u32 nlayers, layers, u32 nparams,
    params, u32 nuserprops, userprops, u32 ?, str category
  layer: u16 flags, i16 priority, u16 control parameter, u16 nsounds, u16 nenvelopes,
         nsounds * {u16 sounddef index, f32 start, f32 length, 12 x u32/f32 instance settings},
         nenvelopes * {i32 parent, str dsp name, u32 dsp parameter, u32 flags, u32 parameter, u32 npoints,
                       npoints * {u32 position, f32 value, u32 curve}, u32 ?, u32 ?}
  param: str name, f32 velocity, f32 min, f32 max, u32 flags, f32 seek speed, u32 nenvelopes, u32 nsustain, u32[]
  userprop: str name, u32 type (0 int, 1 float, 2 string), value
  u32 nsdprops, nsdprops * 64-byte sounddef property blocks (play mode, spawn, volume / pitch randomisation, ...)
  u32 nsounddefs, sounddef: str name, u32 property block, u32 nwaves,
    wave: u32 type (0 = wavetable), u32 weight, str file, str bank, u32 index in bank (FSB subsong), u32 length ms
  then RIFF-like chunks ("comp", "sett", ...), ignored.
Usage: fev.py <file.fev> [--json]   |   fev.py --all [sound dir]   (parse check + summary over every file)"""
import glob, json, os, struct, sys



def sound_dir():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gamepath
    return os.path.join(gamepath.game_dir(), "sound")


class R:
    def __init__(self, d, o=0):
        self.d, self.o = d, o

    def u32(self):
        v, = struct.unpack_from("<I", self.d, self.o); self.o += 4; return v

    def i32(self):
        v, = struct.unpack_from("<i", self.d, self.o); self.o += 4; return v

    def u16(self):
        v, = struct.unpack_from("<H", self.d, self.o); self.o += 2; return v

    def i16(self):
        v, = struct.unpack_from("<h", self.d, self.o); self.o += 2; return v

    def f32(self):
        v, = struct.unpack_from("<f", self.d, self.o); self.o += 4; return round(v, 5)

    def raw(self, n):
        v = self.d[self.o:self.o + n]; self.o += n; return v

    def str(self):
        n = self.u32()
        if n > 4096:
            raise ValueError(f"string length {n} at {self.o - 4:#x}")
        s = self.raw(n)
        return s[:-1].decode("latin-1") if n else ""

    def words(self, n):
        out = []
        for _ in range(n):
            u, = struct.unpack_from("<I", self.d, self.o)
            f, = struct.unpack_from("<f", self.d, self.o)
            self.o += 4
            out.append(round(f, 5) if u and 1e-6 < abs(f) < 1e7 else (u if u < 0x80000000 else u - (1 << 32)))
        return out


def category(r):
    c = dict(name=r.str(), volume=r.f32(), pitch=r.f32(), x=r.words(2))
    c["children"] = [category(r) for _ in range(r.u32())]
    return c


def userprops(r):
    out = {}
    for _ in range(r.u32()):
        name, t = r.str(), r.u32()
        out[name] = r.i32() if t == 0 else r.f32() if t == 1 else r.str()
    return out


def envelope(r):
    e = dict(parent=r.i32(), dsp=r.str(), dsp_param=r.u32(), flags=r.u32(), param=r.u32())
    e["points"] = [(r.u32(), r.f32(), r.u32()) for _ in range(r.u32())]
    e["x"] = r.words(2)
    return e


def sound(r):
    return dict(sounddef=r.u16(), start=r.f32(), length=r.f32(), x=r.words(12))


def layer(r):
    L = dict(flags=r.u16(), priority=r.i16(), param=r.i16())
    ns, ne = r.u16(), r.u16()
    L["sounds"] = [sound(r) for _ in range(ns)]
    L["envelopes"] = [envelope(r) for _ in range(ne)]
    return L


def param(r):
    p = dict(name=r.str(), velocity=r.f32(), min=r.f32(), max=r.f32(), flags=r.u32(), seek=r.f32(),
             nenvelopes=r.u32())
    p["sustain"] = [r.f32() for _ in range(r.u32())]
    return p


EV_FIXED = 25  # words between mode and nlayers (3D block, cone, doppler, speaker levels, reverb, fades, ...)


def event(r, path):
    e = dict(type=r.u32(), name=r.str(), guid=r.raw(16).hex(), volume=r.f32(), pitch=r.f32(), pitch_rand=r.f32(),
             volume_rand=r.f32(), priority=r.u32(), max_playbacks=r.u32(), steal=r.u32(), mode=r.u32())
    e["path"] = path + "/" + e["name"]
    e["x"] = r.words(EV_FIXED)
    e["min_distance"], e["max_distance"] = e["x"][0], e["x"][1]
    if e["type"] == 16:  # simple event: one sound list, no envelopes / parameters / user properties
        e["layers"] = [dict(flags=0, priority=-1, param=-1, envelopes=[],
                            sounds=[sound(r) for _ in range(r.u32())])]
        e["params"], e["props"] = [], {}
    else:
        e["layers"] = [layer(r) for _ in range(r.u32())]
        e["params"] = [param(r) for _ in range(r.u32())]
        e["props"] = userprops(r)
    e["y"] = r.u32()
    e["category"] = r.str()
    return e


def group(r, path, events):
    g = dict(name=r.str())
    p = path + "/" + g["name"]
    g["props"] = userprops(r)
    g["groups"] = [group(r, p, events) for _ in range(r.u32())]
    n = r.u32()
    for _ in range(n):
        events.append(event(r, p))
    g["nevents"] = n
    return g


def parse(path):
    d = open(path, "rb").read()
    r = R(d)
    if r.raw(4) != b"FEV1":
        raise ValueError("not FEV1")
    f = dict(file=os.path.basename(path), version=r.u32(), hx=r.words(2), project=r.str())
    f["banks"] = [dict(flags=r.u32(), max_streams=r.u32(), hash=r.raw(8).hex(), name=r.str()) for _ in range(r.u32())]
    f["category"] = category(r)
    events = []
    f["groups"] = [group(r, "", events) for _ in range(r.u32())]
    f["events"] = events
    f["sdprops"] = [r.words(16) for _ in range(r.u32())]
    sds = []
    for _ in range(r.u32()):
        sd = dict(name=r.str(), props=r.u32(), waves=[])
        for _ in range(r.u32()):
            w = dict(type=r.u32(), weight=r.u32())
            if w["type"] == 0:
                w.update(file=r.str(), bank=r.str(), index=r.u32(), ms=r.u32())
            else:
                raise ValueError(f"wave type {w['type']} at {r.o:#x}")
            sd["waves"].append(w)
        sds.append(sd)
    f["sounddefs"] = sds
    f["tail"] = d[r.o:r.o + 4].decode("latin-1")
    f["tail_offset"] = r.o
    return f


def event_waves(f, e):
    """[(bank, subsong index, file, weight)] reachable from an event"""
    out = []
    for L in e["layers"]:
        for s in L["sounds"]:
            sd = f["sounddefs"][s["sounddef"]]
            out += [(w["bank"], w["index"], w["file"], w["weight"]) for w in sd["waves"]]
    return out


def main():
    a = sys.argv[1:]
    if a and a[0] == "--all":
        files = sorted(glob.glob(os.path.join(a[1] if len(a) > 1 else sound_dir(), "*.fev")))
        ok = ev = sd = wv = 0
        tails = {}
        for p in files:
            try:
                f = parse(p)
            except Exception as ex:
                print(f"FAIL {os.path.basename(p)}: {ex}")
                continue
            ok += 1
            ev += len(f["events"]); sd += len(f["sounddefs"]); wv += sum(len(s["waves"]) for s in f["sounddefs"])
            tails[f["tail"]] = tails.get(f["tail"], 0) + 1
        print(f"{ok}/{len(files)} parsed, {ev} events, {sd} sounddefs, {wv} waves; trailing chunk tags {tails}")
        sys.exit(0 if ok == len(files) else 1)
    f = parse(a[0])
    if "--json" in a:
        json.dump(f, sys.stdout, indent=1)
        return
    print(f"{f['file']}: project {f['project']}, banks {[b['name'] for b in f['banks']]}, {len(f['events'])} events, "
          f"{len(f['sounddefs'])} sounddefs, tail {f['tail']!r} @ {f['tail_offset']:#x}")
    for e in f["events"]:
        w = event_waves(f, e)
        print(f"  {e['path']:40} vol {e['volume']:.3f} max {e['max_playbacks']} 3D {e['min_distance']}-"
              f"{e['max_distance']} params {[p['name'] for p in e['params']]} -> "
              f"{sorted(set((b, i) for b, i, _, _ in w))}")


if __name__ == "__main__":
    main()
