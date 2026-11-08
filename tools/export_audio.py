"""Sound packs for the SPU2 runtime (ps2/proto/src/sound.c): DSR's FMOD banks (sound/*.fsb + *.fev) re-encoded as SPU2
ADPCM, with the TAE sound cues of the characters and the MSB sound emitters of the areas.

Packs (<pak dir>/<name>.pak, tools/pack.py; build/data/snd/), one per FMOD bank group:
  main.pak     frpg_main + fdlc_main (global SE incl. the player's c0000 sounds) + frpg_smain (streamed stingers);
               cue.bin = c0000's TAE sound events, with the Warrior's armor defenseMaterial for b events
  mAA.pak      frpg_mAA + fdlc_mAA + frpg_smAA (music, long ambience: streamed) + frpg_xmAA (5.1 area ambience,
               downmixed, streamed); emit.bin = the Sound events of every mAA_* MSB
  mAA_BB.pak   str.bin alone: the streamed samples only map mAA_BB's music / ambience / area events use (write_pack
               split_aa; Burg plan B8, SND_SPLIT=0 = all in mAA.pak), beside that map's blocks on the disc
  cXXXX.pak    frpg_cXXXX + fdlc_cXXXX for an enemy model placed in one of the maps given; cue.bin = its TAE cues
Each pack: bank.bin (tables), vag.bin (resident ADPCM) and str.bin (streamed samples), both raw entries the IOP reads
in place (vag.bin straight into SPU2 RAM), cue.bin / emit.bin when present.

Event names (DSR, checked on the game's data 2026-11-03): FMOD event = sound type letter + id as 9 digits; TAE / MSB /
EMEVD sound type numbers 0 a, 1 c, 2 f, 3 o, 4 p, 5 s, 6 m, 7 v, 8 x, 9 b, 10 g. x (floor material) and b (armor
material) have no events of their own: they play c events named id // 100 * 100 + material (sound.c resolves them).
Area ambience y events: y + AA BB 00000 (m10_02 -> y100200000), played per map.

Tiers (what goes where; SPU2 RAM is 2 MB):
  resident  every sample <= STREAM_SECONDS (and short mono loops) at RATE, uploaded with its pack
  streamed  longer samples of STREAM_TYPES events, at STREAM_RATE, from disc (3 IOP stream slots)
  dropped   samples used only by s / v / p events no TAE cue names (spells, NPC voices, cutscenes: ASSUMED unneeded
            until those systems exist, TECH_DEBT audio-2), streamed-length v / p samples (voices, cutscenes)
RATE: the highest of RATES at which main + the largest area + the CHR_MIN largest character banks fit SPU_BUDGET.

bank.bin (little-endian)
  header  "DSRB", u32 version 2, u32 nsamples, u32 nevents, u32 nwaves, u32 vag bytes, u32 str bytes, u32 rate
  sample  {u32 offset (vag.bin or str.bin), u32 bytes (all channels), u16 rate, u8 channels,
           u8 flags (1 loops, 2 streamed, 4 dropped, bits 4-7: BB + 1 when in mAA_BB.pak, 15 in every one), u32 chunk (streamed: bytes per channel per chunk)}       16 B
          resident: channel c at offset + c * bytes / channels; streamed: chunks of `chunk` bytes per channel,
          channels interleaved by chunk (src/dsrsnd_rpc.h)
  event   {u32 id, char type, u8 flags (1 loop, 2 3D), u8 nwaves, u8 category (0 SE, 1 music, 2 ambience, 3 voice),
           u16 first wave, u16 max playbacks, f32 volume, f32 volume randomisation, f32 min distance,
           f32 max distance, f32 pitch randomisation}                                                         32 B
          sorted by (type, id)
  wave    {u16 sample, u16 weight}
cue.bin
  header  "DSRC", u32 version 1, u32 nanims, u32 ncues, i32 armor material (-1 none), u32 pad[3]
  anim    {i32 TAE anim id (c0000: a00 ids, then file AA x 10000 + id), u16 first cue, u16 ncues}  sorted by id   8 B
  cue     {f32 start, f32 end, i32 sound id, i8 sound type, u8 kind (0 = 128 CenterBody, 1 = 129 BySlot,
           2 = 130 PlayerVoice), i8 dummy poly (-1), i8 slot (-1)}                                       16 B
emit.bin
  header  "DSRE", u32 version 1, u32 n, u32 pad
  emitter {i32 entity id, i32 sound id, u8 sound type, u8 shape (MSB1: 0 point 1 circle 2 sphere 3 cylinder 4 rect
           5 box), u8 map (BB of mAA_BB), u8 pad, f32 pos[3], f32 rot y (rad), f32 dims[3] (radius / width, depth,
           height)}                                                                                          40 B
Usage: export_audio.py <pak dir> <work dir> <map id> [map id ...]  (the packs those maps need; work dir: the files
                                                                   packed, one directory per pack)
       Decoding and encoding run in DSR_AUDIO_JOBS worker processes (default: every core, at most 8); the packs are
       the same bytes whatever the count (each sample is encoded alone, results are written in order). Encoded
       samples are cached across exports and trees (cache(), SND_CACHE=0 = off): the packs are the same bytes either way.
       export_audio.py --size-only <map id> [...]                 (plan + sizes, no encoding)"""
import glob, os, re, struct, sys, time
import multiprocessing

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "adpcm"))
import adpcm, fev, fsb4, gamepath, pack, souls

SPU_RAM = 2 * 1024 * 1024
SPU_SYSTEM = 0x5040    # bottom of SPU2 RAM: core input / output areas (libsd); sound.c starts its heap here
SPU_STREAM = 3 * 2 * 2 * 8192  # sound.c: 3 stream slots x 2 channels x 2 ring halves x 8 KB chunks
SPU_BUDGET = SPU_RAM - SPU_SYSTEM - SPU_STREAM  # no reverb work area: sound.c runs without reverb (TECH_DEBT audio-7)
RATES = (22050, 16000, 11025, 8000)
STREAM_RATE = 22050    # ASSUMED stream rate (CD budget: 12.6 KB/s per mono channel)
STREAM_CHUNK = 8192    # bytes per channel per IOP read (dsrsnd_rpc.h SND_STREAM_CHUNK_MAX)
STREAM_SECONDS = 4.0   # ASSUMED: one-shots longer than this stream from disc
STREAM_TYPES = set("macosy")  # streamed sample kept for these event types (no voices v, cutscenes p)
DEMAND_TYPES = set("svp")  # main-bank samples only these use are dropped unless a TAE cue names the event
CHR_MIN = 2            # ASSUMED: character banks the budget leaves room for next to main + area
ALIGN = 64
TYPES = "acfopsmvxbg"  # sound type number -> event letter
WARRIOR = 3000         # CharaInitParam row of the player (tools/export_player.py)


def vag_bytes(n, src, rate, ch, loop):
    m = n if src <= rate else int(n * rate / src)
    blocks = (m + 27) // 28 + (0 if loop else 1)
    return ch * ((blocks * 16 + ALIGN - 1) // ALIGN * ALIGN)


def bank_paths(snd, base):
    """existing (fsb, fev) for a bank base name in either the frpg_ or fdlc_ set"""
    out = []
    for pre in ("frpg_", "fdlc_"):
        f = os.path.join(snd, pre + base + ".fsb")
        if os.path.exists(f):
            out.append((f, os.path.join(snd, pre + base + ".fev")))
    return out


def event_id(name):
    m = re.fullmatch(r"([a-z])(\d+)", name)
    return (m.group(1), int(m.group(2))) if m and int(m.group(2)) < 1 << 32 else None


def area_models(game, map_id):
    M = souls.msb1(souls.read(f"{game}/map/MapStudio/{map_id}.msb"))
    return sorted({p["model"] for p in M["parts"] if p["type"] in (2, 10) and p["model"] and p["model"] != "c0000"})


# ---- TAE cues ----

def tae_cues(anibnd_path, prefix_ids):
    """{anim id: [(start, end, sound id, sound type, kind, dummy, slot)]} from every TAE in an anibnd.
    prefix_ids: c0000's a00 / aNN file naming (id = NN x 10000 + anim), else the TAE's own anim ids."""
    out = {}
    for _, n, d in souls.bnd3(souls.read(anibnd_path)):
        if not n.lower().endswith(".tae"):
            continue
        base = souls.basename(n)[:-4].lower()
        m = re.fullmatch(r"a(\d\d)", base)
        if prefix_ids and not m:
            continue
        off = int(m.group(1)) * 10000 if prefix_ids and m else 0
        for aid, a in souls.tae(d).items():
            for etype, s, e, p in a["events"]:
                if etype not in (128, 129, 130) or len(p) < 2:
                    continue
                st, sid = struct.unpack("<2i", struct.pack("<2I", p[0], p[1]))
                dmy = slot = -1
                if etype != 128 and len(p) >= 4:  # only these words are real (the reader's last block overruns)
                    dmy, slot = struct.unpack("<2i", struct.pack("<2I", p[2], p[3]))
                if 0 <= st < len(TYPES):
                    out.setdefault(off + aid, []).append((s, e, sid, st, etype - 128, max(-1, min(127, dmy)),
                                                          max(-1, min(127, slot))))
    return out


def cue_names(cues, material=-1):
    """FMOD event names a cue table can play (x / b: every material variant)"""
    names = set()
    for lst in cues.values():
        for _, _, sid, st, *_ in lst:
            t = TYPES[st]
            if t in "xb":
                base = sid // 100 * 100
                names |= {f"c{base + k:09d}" for k in range(100)} | {f"c{sid:09d}"}
            else:
                names.add(f"{t}{sid:09d}")
    return names


def cue_blob(cues, material):
    anims, rows = [], []
    for aid in sorted(cues):
        lst = sorted(cues[aid])
        anims.append(struct.pack("<iHH", aid, len(rows), len(lst)))
        rows += [struct.pack("<ffibBbb", s, e, sid, st, kind, dmy, slot) for s, e, sid, st, kind, dmy, slot in lst]
    return struct.pack("<4sIIIi3I", b"DSRC", 1, len(anims), len(rows), material, 0, 0, 0) + b"".join(anims) + b"".join(rows)


def warrior_material(game):
    """defenseMaterial of the Warrior's body armor (b events: c id // 100 * 100 + material)"""
    P = souls.game_params(game)
    init = P["CharaInitParam"][WARRIOR][1]
    row = P["EquipParamProtector"].get(init["equip_Armer"])
    return int(row[1]["defenseMaterial"]) if row else -1


# ---- MSB emitters ----

SHAPES = {"Point": 0, "Circle": 1, "Sphere": 2, "Cylinder": 3, "Rectangle": 4, "Box": 5}


def emitters(game, aa):
    rows = []
    for msb in sorted(glob.glob(f"{game}/map/MapStudio/m{aa}_*.msb")):
        bb = int(os.path.basename(msb)[4:6])
        M = souls.msb1(souls.read(msb))
        regions = {r["name"]: r for r in M["regions"]}
        for e in M["events"]:
            if e["type"] != 1 or e["sound_type"] < 0 or e["sound_type"] >= len(TYPES):
                continue
            r = regions.get(e["region"])
            if not r or r["shape"] not in SHAPES:
                continue
            dims = (r.get("radius", r.get("width", 0)), r.get("depth", 0), r.get("height", 0))
            if r["shape"] == "Cylinder":
                dims = (r["radius"], 0, r["height"])
            ent = e["entity_id"] if e["entity_id"] > 0 else r["entity_id"]
            rows.append(struct.pack("<iiBBBx3ff3f", ent, e["sound_id"], e["sound_type"], SHAPES[r["shape"]], bb,
                                    *r["pos"], np.radians(r["rot"][1]), *dims))
    return struct.pack("<4sIII", b"DSRE", 1, len(rows), 0) + b"".join(rows), len(rows)


# ---- banks ----

class Group:
    """One pack's banks: FSBs + their FEV events, wave refs resolved, tier of every sample"""

    def __init__(self, name, paths, wanted):
        self.name = name
        self.banks = [(fsb4.FSB4(f), e) for f, e in paths]
        self.by_name = {b.name: b for b, _ in self.banks}
        self.events, self.users, self.loops, self.stale = [], {}, set(), 0
        for b, fev_path in self.banks:
            if not os.path.exists(fev_path):
                continue
            F = fev.parse(fev_path)
            for e in F["events"]:
                eid = event_id(e["name"])
                refs, loop = [], False
                for L in e["layers"]:
                    for si in L["sounds"]:
                        loop |= si["x"][1] == 0  # sound instance loop mode: 0 = loop, 1 = one-shot
                        sd = F["sounddefs"][si["sounddef"]]
                        for w in sd["waves"]:
                            wb = self.by_name.get(w["bank"])
                            if not wb:
                                continue
                            i = w["index"]
                            if i >= len(wb.samples):  # stale game data (TECH_DEBT audio-3)
                                self.stale += 1
                                sm = wb.by_name.get(os.path.splitext(os.path.basename(w["file"]))[0].lower())
                                if not sm:
                                    continue
                                i = sm.index
                            refs.append(((wb.name, i), w["weight"]))
                if not eid:
                    continue
                for k, _ in refs:
                    u = self.users.setdefault(k, set())
                    u.add(e["name"][:1])
                    if e["name"] in wanted:
                        u.add("*")  # named by a TAE cue: never dropped as on-demand
                    if loop:
                        self.loops.add(k)
                self.events.append((eid, e, loop, refs))
        self.tier = {}
        for b, _ in self.banks:
            for s in b.samples:
                k = (b.name, s.index)
                users = self.users.get(k, set())
                secs = s.length / s.freq
                if secs > STREAM_SECONDS and not (k in self.loops and s.channels == 1 and secs < 2 * STREAM_SECONDS):
                    self.tier[k] = "stream" if users & STREAM_TYPES else "drop"
                elif users and users <= DEMAND_TYPES:
                    self.tier[k] = "drop"
                elif not users:
                    self.tier[k] = "drop"  # no event plays it
                else:
                    self.tier[k] = "res"

    def resident_bytes(self, rate):
        return sum(vag_bytes(s.length, s.freq, rate, s.channels, (b.name, s.index) in self.loops)
                   for b, _ in self.banks for s in b.samples if self.tier[(b.name, s.index)] == "res")

    def stream_bytes(self):
        n = 0
        for b, _ in self.banks:
            for s in b.samples:
                if self.tier[(b.name, s.index)] == "stream":
                    m = int(s.length * min(1, STREAM_RATE / s.freq))
                    per = ((m + 27) // 28 * 16 + STREAM_CHUNK - 1) // STREAM_CHUNK * STREAM_CHUNK
                    n += min(2, s.channels) * per
        return n


def downmix(pcm):
    """5.1 (L R C LFE Ls Rs, FMOD order) -> stereo; ASSUMED ITU coefficients, LFE dropped (TECH_DEBT audio-5)"""
    x = pcm.astype(np.float32)
    if x.shape[1] == 6:
        l = x[:, 0] + 0.707 * x[:, 2] + 0.707 * x[:, 4]
        r = x[:, 1] + 0.707 * x[:, 2] + 0.707 * x[:, 5]
        x = np.stack([l, r], 1) / 2.414
    elif x.shape[1] > 2:
        x = x[:, :2]
    return np.clip(np.round(x), -32768, 32767).astype(np.int16)


_FSB = {}  # worker: open banks by path


def _encode(pcm, s, tier, loop, rate):
    """decoded sample -> (ADPCM blobs per channel, SNR of channel 0 or None)"""
    if tier == "res":
        pcm = adpcm.resample(pcm, s.freq, min(rate, s.freq))
        chans = [adpcm.encode(pcm[:, c], 0 if loop else None) for c in range(s.channels)]
        return chans, adpcm.snr(pcm[:, 0], adpcm.decode(chans[0]))
    pcm = adpcm.resample(downmix(pcm) if s.channels > 2 else pcm, s.freq, min(STREAM_RATE, s.freq))
    return [adpcm.encode(pcm[:, c], None) for c in range(pcm.shape[1])], None


_CACHE = []


def cache():
    """(dir, key hash) of the encoded-sample cache, or None: SND_CACHE (default <main tree>/build/cache/snd, shared by
    every tree and build dir like the texture cache; 0 = off). The key covers the encoder (adpcm.py, vagenc.c, _encode,
    downmix, STREAM_RATE, numpy) and, per sample, its decoded PCM and the arguments, so a hit is what _encode returns:
    an export after a change elsewhere (souls.py, area.py, a new map) encodes only samples it has not seen."""
    if not _CACHE:
        import hashlib, inspect
        d = os.environ.get("SND_CACHE")
        if d == "0":
            _CACHE.append(None)
            return None
        if not d:
            import simplify_pool
            d = os.path.join(os.environ.get("DSR_CACHE") or os.path.join(simplify_pool.main_tree(), "build", "cache"),
                             "snd")
        h = hashlib.sha256(b"snd-cache-1")
        for f in ("adpcm.py", "vagenc.c"):
            h.update(open(os.path.join(HERE, "adpcm", f), "rb").read())
        h.update(repr((inspect.getsource(_encode), inspect.getsource(downmix), STREAM_RATE, np.__version__)).encode())
        _CACHE.append((d, h))
    return _CACHE[0]


def encode_sample(job):
    """one sample -> (ADPCM blobs per channel, SNR of channel 0 or None), from the cache when it has it; runs in a
    worker process"""
    import pickle
    path, index, tier, loop, rate = job
    b = _FSB.get(path) or _FSB.setdefault(path, fsb4.FSB4(path))
    s = b.samples[index]
    pcm = b.decode(s)  # ~1 % of the encode time: keying on the PCM keeps the cache right across decoder changes
    c = cache()
    if c is None:
        return _encode(pcm, s, tier, loop, rate)
    k = c[1].copy()
    k.update(repr((pcm.shape, s.freq, s.channels, tier, bool(loop), rate)).encode())
    k.update(np.ascontiguousarray(pcm).tobytes())
    key = k.hexdigest()
    p = os.path.join(c[0], key[:2], key)
    try:
        with open(p, "rb") as f:
            return pickle.load(f)
    except (OSError, ValueError, EOFError, pickle.UnpicklingError):
        pass
    r = _encode(pcm, s, tier, loop, rate)
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        tmp = f"{p}.{os.getpid()}.tmp"
        with open(tmp, "wb") as f:
            pickle.dump(r, f, protocol=pickle.HIGHEST_PROTOCOL)
        os.replace(tmp, p)
    except OSError as e:
        print(f"export_audio: cache write failed ({e})", flush=True)
    return r


POOL = None  # main(): worker pool, None = encode in this process (Pyodide has no processes)


def start_pool():
    global POOL
    n = int(os.environ.get("DSR_AUDIO_JOBS") or min(os.cpu_count() or 1, 8))
    adpcm.lib()  # build libvagenc.so here, once, before the workers would each try
    c = cache()  # key prefix once, inherited by the workers
    if n > 1:
        try:
            POOL = multiprocessing.get_context("fork").Pool(n)
        except Exception:  # no processes here (Pyodide): encode in this one
            POOL = None
    print(f"encoding in {n if POOL else 1} process(es) (DSR_AUDIO_JOBS); sample cache {c[0] if c else 'off'}"
          " (SND_CACHE)", flush=True)


MAP_TYPES = set("may")  # events whose id is AA BB xxxxx: music, area SE, area ambience of one map (map_of)
SPLIT = os.environ.get("SND_SPLIT", "1") != "0"  # Burg plan B8 (default on; SND_SPLIT=0: one str.bin per area)


def map_of(typ, num, aa):
    """map BB an area bank's event belongs to (music, ambience, area SE of mAA_BB), else None (shared)"""
    return num // 100000 % 100 if typ in MAP_TYPES and num // 10000000 == aa else None


def sample_maps(g, aa):
    """(bank, sample) -> the one map BB whose events alone use it (streamed samples of an area bank), else absent"""
    own = {}
    for (typ, num), e, loop, refs in g.events:
        bb = map_of(typ, num, aa)
        for k, _ in refs:
            own.setdefault(k, set()).add(bb)
    return {k: next(iter(v)) for k, v in own.items() if len(v) == 1 and None not in v and next(iter(v)) < 14}


def write_pack(out, work, name, g, rate, cue=None, emit=None, split_aa=None, split_maps=None):
    """split_aa (an area bank, SPLIT): each streamed sample that only one map's events use goes into that map's own
    pack mAA_BB.pak (str.bin alone), placed on the disc beside the map's blocks (tools/iso9660.py) so a walk there
    reads its music and ambience inside the drive's fast seek window; the sample's flags carry BB + 1 in bits 4-7
    (src/sound.c opens the stream from that pack). The rest stay in mAA.pak's str.bin."""
    t0 = time.time()
    samples, vag, strm, sidx = [], bytearray(), bytearray(), {}
    owner = sample_maps(g, split_aa) if split_aa is not None and SPLIT else {}
    mstrm = {}  # BB -> str.bin of mAA_BB.pak (after the shared streams, copied at its start)
    if owner:  # every map of the area that has a block, or owns a stream, gets a pack
        for bb in sorted(set(owner.values()) | set(split_maps or ())):
            mstrm[bb] = None
    snr = []
    jobs = [(b.path, s.index, g.tier[(b.name, s.index)], (b.name, s.index) in g.loops, rate)
            for b, _ in g.banks for s in b.samples if g.tier[(b.name, s.index)] != "drop"]
    print(f"  {name:8} encoding {len(jobs)} samples ...", flush=True)
    results = POOL.imap(encode_sample, jobs) if POOL else map(encode_sample, jobs)
    done, step = 0, max(1, len(jobs) // 4)
    for b, _ in g.banks:
        for s in b.samples:
            k = (b.name, s.index)
            t = g.tier[k]
            loop = k in g.loops
            sidx[k] = len(samples)
            if t == "drop":
                samples.append(struct.pack("<IIHBBI", 0, 0, min(rate, s.freq), s.channels, 4 | int(loop), 0))
                continue
            chans, q = next(results)
            done += 1
            if done % step == 0 and done < len(jobs) and time.time() - t0 > 5:
                print(f"  {name:8} {done}/{len(jobs)} samples, {time.time() - t0:.0f} s", flush=True)
            if t == "res":
                off = len(vag)
                if len(snr) < 200:
                    snr.append(q)
                for a in chans:
                    vag += a + b"\0" * (-len(a) % ALIGN)
                samples.append(struct.pack("<IIHBBI", off, len(vag) - off, min(rate, s.freq), s.channels,
                                           int(loop), 0))
            else:
                n = max(len(a) for a in chans)
                n += -n % STREAM_CHUNK
                chans = [a + b"\0" * (n - len(a)) for a in chans]
                bb = owner.get(k)
                if bb is not None and mstrm.get(bb) is None:
                    mstrm[bb] = bytearray()
                dst = strm if bb is None else mstrm[bb]
                off = len(dst)
                for o in range(0, n, STREAM_CHUNK):
                    for a in chans:
                        dst += a[o:o + STREAM_CHUNK]
                # a shared one (bb None) is also at the start of every map's pack, same offset: flag 15 = the map
                # the player is in (src/sound.c), so no stream of a walk reads mAA.pak
                samples.append(struct.pack("<IIHBBI", off, len(dst) - off, min(STREAM_RATE, s.freq), len(chans),
                                           2 | int(loop) | ((15 if mstrm else 0) if bb is None else (bb + 1) << 4),
                                           STREAM_CHUNK))
    for i, smp in enumerate(samples):  # map-own streams: after the shared ones in their map's pack
        off, nb, r, ch, fl, ck = struct.unpack("<IIHBBI", smp)
        if fl & 2 and 0 < fl >> 4 < 15:
            samples[i] = struct.pack("<IIHBBI", off + len(strm), nb, r, ch, fl, ck)
    cat = {"music": 1, "ambience": 2, "voice": 3}
    evs = []
    for (typ, num), e, loop, refs in g.events:
        refs = [(sidx[k], w) for k, w in refs if k in sidx][:255]
        evs.append(((typ, num), e, loop, refs))
    evs.sort(key=lambda r: r[0])
    evb, waves = [], []
    for (typ, num), e, loop, refs in evs:
        first = len(waves)
        waves += [struct.pack("<HH", i, min(w, 0xFFFF)) for i, w in refs]
        c = e["category"].lower().rsplit("/", 1)[-1]
        flags = int(loop) | (2 if e["mode"] & 0x10 else 0)
        evb.append(struct.pack("<IcBBBHHfffff", num, typ.encode(), flags, len(refs), cat.get(c, 0), first,
                               min(e["max_playbacks"], 0xFFFF), e["volume"], e["volume_rand"], e["min_distance"],
                               e["max_distance"], e["pitch_rand"]))
    hdr = struct.pack("<4s7I", b"DSRB", 2, len(samples), len(evb), len(waves), len(vag), len(strm), rate)
    gen = os.path.join(work, name)
    os.makedirs(gen, exist_ok=True)
    files = {"bank.bin": hdr + b"".join(samples) + b"".join(evb) + b"".join(waves), "vag.bin": bytes(vag) or b"\0" * 64,
             "str.bin": bytes(strm) or b"\0" * 64}
    if cue is not None:
        files["cue.bin"] = cue
    if emit is not None:
        files["emit.bin"] = emit
    args = []
    for f, d in files.items():
        p = os.path.join(gen, f)
        with open(p, "wb") as fh:
            fh.write(d)
        args.append(f"{f}={'raw:' if f in ('str.bin', 'vag.bin') else ''}{p}")
    os.makedirs(out, exist_ok=True)
    pack.main([os.path.join(out, name + ".pak")] + args, log=lambda *a: None)  # in process: Pyodide has no subprocess
    for bb, d in sorted(mstrm.items()):  # the maps' own streams (split_aa), after a copy of the shared ones
        sub = f"{name}_{bb:02d}"
        p = os.path.join(gen, f"str_{bb:02d}.bin")
        with open(p, "wb") as fh:
            fh.write(bytes(strm) + bytes(d or b""))
        pack.main([os.path.join(out, sub + ".pak"), f"str.bin=raw:{p}"], log=lambda *a: None)
        print(f"  {sub:8} str {len(d) / 1048576:6.1f} M (the map's own music / ambience)", flush=True)
    print(f"  {name:8} {len(samples):5} samples ({sum(1 for v in g.tier.values() if v == 'res')} resident, "
          f"{sum(1 for v in g.tier.values() if v == 'stream')} streamed, {sum(1 for v in g.tier.values() if v == 'drop')}"
          f" dropped), {len(evb):5} events, vag {len(vag) / 1024:7.0f} K, str {len(strm) / 1048576:6.1f} M,"
          f" SNR {float(np.median(snr)) if snr else 0:4.1f} dB, {time.time() - t0:4.0f} s", flush=True)


def main():
    a = sys.argv[1:]
    size_only = a and a[0] == "--size-only"
    if size_only:
        a = a[1:]
        out = work = None
    else:
        out, work = a.pop(0), a.pop(0)
    maps = a
    if not maps:
        sys.exit(__doc__)
    game = gamepath.game_dir()
    snd = os.path.join(game, "sound")
    areas = sorted({m[1:3] for m in maps})
    models = sorted({c for m in maps for c in area_models(game, m)})
    chr_dir = os.path.join(game, "chr")
    cues = {"c0000": tae_cues(f"{chr_dir}/c0000.anibnd.dcx", True)}
    for c in models:
        p = f"{chr_dir}/{c}.anibnd.dcx"
        cues[c] = tae_cues(p, False) if os.path.exists(p) else {}
    material = warrior_material(game)
    wanted = set()
    for c, cu in cues.items():
        wanted |= cue_names(cu)
    groups = {"main": Group("main", bank_paths(snd, "main") + bank_paths(snd, "smain"), wanted)}
    for aa in areas:
        groups["m" + aa] = Group("m" + aa, bank_paths(snd, "m" + aa) + bank_paths(snd, "sm" + aa) +
                                 bank_paths(snd, "xm" + aa), wanted)
    chr_groups = {c: Group(c, bank_paths(snd, c), wanted) for c in models if bank_paths(snd, c)}
    print(f"SPU2 budget for resident samples: {SPU_BUDGET} B; maps {' '.join(maps)}; {len(chr_groups)} character banks"
          f" ({' '.join(c for c in models if c not in chr_groups) or 'none'} without one)")
    rate = RATES[-1]
    for r in RATES:
        area_max = max(groups["m" + aa].resident_bytes(r) for aa in areas)
        chr_sz = sorted((g.resident_bytes(r) for g in chr_groups.values()), reverse=True)
        need = groups["main"].resident_bytes(r) + area_max + sum(chr_sz[:CHR_MIN])
        print(f"  {r:5} Hz: main {groups['main'].resident_bytes(r) / 1024:6.0f} K + area {area_max / 1024:6.0f} K + "
              f"{CHR_MIN} chr {sum(chr_sz[:CHR_MIN]) / 1024:5.0f} K = {need / 1024:6.0f} K"
              f"{'  fits' if need <= SPU_BUDGET else ''}")
        if need <= SPU_BUDGET:
            rate = r
            break
    print(f"rate {rate} Hz; streamed: " + ", ".join(f"{n} {g.stream_bytes() / 1048576:.1f} MB" for n, g in groups.items()))
    if size_only:
        return
    t0 = time.time()
    start_pool()
    write_pack(out, work, "main", groups["main"], rate, cue=cue_blob(cues["c0000"], material))
    for aa in areas:
        blob, n = emitters(game, aa)
        write_pack(out, work, "m" + aa, groups["m" + aa], rate, emit=blob, split_aa=int(aa),
                   split_maps={int(m[4:6]) for m in maps if m[1:3] == aa})
    for c, g in chr_groups.items():
        write_pack(out, work, c, g, rate, cue=cue_blob(cues.get(c, {}), -1))
    for c in models:  # characters with TAE cues but no bank of their own (their cues name main-bank events)
        if c not in chr_groups and cues.get(c):
            write_pack(out, work, c, Group(c, [], wanted), rate, cue=cue_blob(cues[c], -1))
    if POOL:
        POOL.close()
        POOL.join()
    print(f"sound: {len(groups) + len(chr_groups)} packs in {time.time() - t0:.0f} s", flush=True)


if __name__ == "__main__":
    main()
