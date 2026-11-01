"""Per-area sound bank export (prototype for a later SPU2 audio runtime): which DSR banks an area needs, their events,
and the samples as SPU2 ADPCM sized against the 2 MB of SPU2 RAM.

Banks per area (sound/ naming; the area is the map's first two digits, mAA):
  resident (SPU RAM): frpg_main + fdlc_main (global SE incl. the player's c0000 sounds), frpg_mAA (+ fdlc_mAA),
                      frpg_cXXXX / fdlc_cXXXX for every enemy model placed in the map's MSB (c0000 excluded)
  on demand:          frpg_p1A#### (cutscene banks of area A = AA - 10), one at a time
  streamed:           frpg_smAA (voices, music, long ambience), frpg_smain, frpg_xmAA (5.1 ambience) -- from disc,
                      sized here at the stream rate but never counted against SPU RAM
Events come from the bank's own .fev (tools/fev.py); waves resolve by FSB subsong index as FMOD does, falling back to
the file name when the index is out of range (stale game data, TECH_DEBT audio-3).
Rate: the highest of RATES at which the resident set fits SPU_BUDGET (samples already below it keep their rate).

Output per area <out>/snd_<map>.bin (little-endian):
  header  "DSRS", u32 version 1, u32 nsamples, u32 nevents, u32 nwaves, u32 vag bytes, u32 rate, u32 pad
  sample  {u32 vag offset, u32 bytes, u16 rate, u8 channels, u8 flags (1 = loops), u32 pad}       16 B
  event   {u32 id, char type, u8 flags (1 loop, 2 3D), u8 play mode, u8 nwaves, u16 first wave, u16 max playbacks,
           f32 volume, f32 pitch randomisation, f32 min distance, f32 max distance, f32 play distance} 32 B
  wave    {u16 sample, u16 weight}
  vag     SPU2 ADPCM blocks (tools/adpcm), each sample 64-byte aligned (SPU transfer granularity); stereo samples
          are two consecutive mono streams
plus snd_<map>.json (names, bank origin, sizes) and one summary table on stdout.
Usage: export_audio.py <out dir> [map id ...]   (default: every m1x map in map/MapStudio)
       export_audio.py <out dir> --size-only [map id ...]   (plan + sizes, no encoding)"""
import glob, json, os, re, struct, sys, time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "adpcm"))
import adpcm, fev, fsb4, gamepath, souls

SPU_RAM = 2 * 1024 * 1024
SPU_SYSTEM = 0x5010    # ASSUMED: bottom of SPU2 RAM (core input/output areas); ps2sdk audsrv uploads above it
SPU_REVERB = 0x10000   # ASSUMED: one core's reverb work area (hall-sized); the real size depends on the mode used
SPU_STREAM = 0x8000    # ASSUMED: double buffers for 2 stereo disc streams (music/voice + ambience), 4 KB per half
SPU_BUDGET = SPU_RAM - SPU_SYSTEM - SPU_REVERB - SPU_STREAM
RATES = (22050, 16000, 11025, 8000)
STREAM_RATE = 22050    # ASSUMED stream rate for sm/xm/smain material (CD budget: 12.6 KB/s per mono channel)
ALIGN = 64


def vag_bytes(n, src, rate, ch, loop):
    m = n if src <= rate else int(n * rate / src)
    blocks = (m + 27) // 28 + (0 if loop else 1)
    return ch * ((blocks * 16 + ALIGN - 1) // ALIGN * ALIGN)


def area_models(game, map_id):
    b = souls.read(f"{game}/map/MapStudio/{map_id}.msb")
    M = souls.msb1(b)
    return sorted({p["model"] for p in M["parts"] if p["type"] in (2, 10) and p["model"] and p["model"] != "c0000"})


def bank_paths(snd, base):
    """existing (fsb, fev) for a bank base name in either the frpg_ or fdlc_ set"""
    out = []
    for pre in ("frpg_", "fdlc_"):
        f = os.path.join(snd, pre + base + ".fsb")
        if os.path.exists(f):
            out.append((f, os.path.join(snd, pre + base + ".fev")))
    return out


def plan(game, snd, map_id):
    aa = map_id[1:3]
    models = area_models(game, map_id)
    resident, missing = [], []
    for base in ["main", "m" + aa] + models:
        p = bank_paths(snd, base)
        resident += p
        if not p and base.startswith("c"):
            missing.append(base)
    a = int(aa) - 10
    cutscene = sorted(glob.glob(os.path.join(snd, f"frpg_p1{a}????.fsb")) + glob.glob(os.path.join(snd, f"fdlc_p1{a}????.fsb")))
    streamed = [f for f in (os.path.join(snd, f"frpg_sm{aa}.fsb"), os.path.join(snd, "frpg_smain.fsb"),
                            os.path.join(snd, f"frpg_xm{aa}.fsb"), os.path.join(snd, f"fdlc_sm{aa}.fsb"))
                if os.path.exists(f)]
    return models, missing, resident, cutscene, streamed


def bank_size(fsb_path, rate):
    b = fsb4.FSB4(fsb_path)
    return sum(vag_bytes(s.length, s.freq, rate, s.channels, False) for s in b.samples), b


def event_id(name):
    m = re.fullmatch(r"([a-z])(\d+)", name)
    return (m.group(1), int(m.group(2))) if m and int(m.group(2)) < 1 << 32 else None


TIER_CORE, TIER_CHR, TIER_DEMAND, TIER_STREAM = range(4)
TIER_NAMES = ("core", "chr", "demand", "stream")
STREAM_SECONDS = 4.0   # ASSUMED: one-shots longer than this (and every stereo sample this long) stream from disc
DEMAND_TYPES = set("svp")  # ASSUMED: main-bank samples only used by s (spell / SFX), v, p events load on demand
CHR_RESIDENT = 4       # ASSUMED: character banks resident at once (enemy types near the player, like chr streaming)
DEMAND_POOL = 0x30000  # ASSUMED: SPU bytes kept for on-demand spell / SFX samples


def tier_of(bank, s, users):
    if s.length / s.freq > STREAM_SECONDS:
        return TIER_STREAM
    if re.search(r"_c\d{4}$", bank):
        return TIER_CHR
    if bank.endswith("_main") and users and users <= DEMAND_TYPES:
        return TIER_DEMAND
    return TIER_CORE


def fit(row_sizes, chr_sizes):
    """SPU bytes needed: core + the CHR_RESIDENT largest character banks + the on-demand pool"""
    return row_sizes[TIER_CORE] + sum(sorted(chr_sizes, reverse=True)[:CHR_RESIDENT]) + DEMAND_POOL


def export(out, game, snd, map_id, size_only):
    t0 = time.time()
    models, missing, resident, cutscene, streamed = plan(game, snd, map_id)
    banks = [(fsb4.FSB4(f), e) for f, e in resident]
    by_name = {b.name: b for b, _ in banks}

    # events first: which event types use each sample, which samples loop
    users, loops = {}, set()
    events, ev_json, stale = [], [], 0
    for b, fev_path in banks:
        if not os.path.exists(fev_path):
            continue
        F = fev.parse(fev_path)
        for e in F["events"]:
            eid = event_id(e["name"])
            refs, loop, mode = [], False, 0
            for L in e["layers"]:
                for si in L["sounds"]:
                    loop |= si["x"][1] == 0  # sound instance loop mode: 0 = loop, 1 = one-shot
                    sd = F["sounddefs"][si["sounddef"]]
                    mode = F["sdprops"][sd["props"]][0] if sd["props"] < len(F["sdprops"]) else 0
                    for w in sd["waves"]:
                        wb = by_name.get(w["bank"])
                        if not wb:
                            continue
                        i = w["index"]
                        if i >= len(wb.samples):
                            stale += 1
                            sm = wb.by_name.get(os.path.splitext(os.path.basename(w["file"]))[0].lower())
                            if not sm:
                                continue
                            i = sm.index
                        refs.append(((wb.name, i), w["weight"]))
                        users.setdefault((wb.name, i), set()).add(e["name"][:1])
                        if loop:
                            loops.add((wb.name, i))
            if eid and refs:
                events.append((eid, e, loop, mode, refs))

    # sample table and tiers
    order, sizes = [], {r: [0, 0, 0, 0] for r in RATES}
    chr_sizes = {r: {} for r in RATES}
    for bi, (b, _) in enumerate(banks):
        for s in b.samples:
            key = (b.name, s.index)
            t = tier_of(b.name, s, users.get(key, set()))
            if key in loops and t == TIER_STREAM and s.channels == 1 and s.length / s.freq < 2 * STREAM_SECONDS:
                t = TIER_CORE  # short ambient loops stay resident
            order.append((bi, b, s, t))
            for r in RATES:
                n = vag_bytes(s.length, s.freq, r, s.channels, key in loops)
                sizes[r][t] += n
                if t == TIER_CHR:
                    chr_sizes[r][b.name] = chr_sizes[r].get(b.name, 0) + n
    rate = next((r for r in RATES if fit(sizes[r], chr_sizes[r].values()) <= SPU_BUDGET), RATES[-1])
    row = dict(map=map_id, models=len(models), missing=missing, banks=len(banks), samples=len(order),
               tiers={r: sizes[r] for r in RATES}, need={r: fit(sizes[r], chr_sizes[r].values()) for r in RATES},
               chr_max=max(chr_sizes[rate].values() or [0]), rate=rate,
               cutscene_max=max([bank_size(f, rate)[0] for f in cutscene] or [0]),
               streamed=sum(bank_size(f, STREAM_RATE)[0] for f in streamed),
               streamed_min=sum(s.length / s.freq for f in streamed for s in fsb4.FSB4(f).samples) / 60)
    if size_only:
        return row

    # encode, one sample in memory at a time; stream-tier samples at STREAM_RATE
    sidx, table, vag, names, snr = {}, [], bytearray(), [], []
    for bi, b, s, t in order:
        key = (b.name, s.index)
        r = min(STREAM_RATE if t == TIER_STREAM else rate, s.freq)
        pcm = adpcm.resample(b.decode(s), s.freq, r)
        off = len(vag)
        for c in range(s.channels):
            a_ = adpcm.encode(pcm[:, c], 0 if key in loops else None)
            if c == 0 and len(snr) < 300:
                snr.append(adpcm.snr(pcm[:, 0], adpcm.decode(a_)))
            vag += a_
            vag += b"\0" * (-len(vag) % ALIGN)
        sidx[key] = len(table)
        flags = (1 if key in loops else 0) | t << 1
        table.append(struct.pack("<IIHBBHH", off, len(vag) - off, r, s.channels, flags, bi, 0))
        names.append(dict(bank=b.name, name=s.name, rate=r, channels=s.channels, bytes=len(vag) - off,
                          tier=TIER_NAMES[t], loop=key in loops))

    waves, evb = [], []
    for (typ, num), e, loop, mode, refs in events:
        first = len(waves)
        waves += [struct.pack("<HH", sidx[k], w) for k, w in refs]
        flags = (1 if loop else 0) | (2 if e["mode"] & 0x10 else 0)
        evb.append(struct.pack("<IcBBBHHfffff", num, typ.encode(), flags, mode & 255, min(len(refs), 255), first,
                               e["max_playbacks"], e["volume"], e["volume_rand"], e["min_distance"], e["max_distance"],
                               float(e["props"].get("playDistance", e["max_distance"]))))
        ev_json.append(dict(id=e["name"], waves=len(refs), loop=loop, volume=e["volume"],
                            max_distance=e["max_distance"], category=e["category"]))
    bank_tab = b"".join(struct.pack("<16s", b.name.encode()) for b, _ in banks)
    hdr = struct.pack("<4s8I", b"DSRS", 1, len(banks), len(table), len(evb), len(waves), len(vag), rate, 0)
    path = os.path.join(out, f"snd_{map_id}.bin")
    with open(path, "wb") as f:
        f.write(hdr + bank_tab + b"".join(table) + b"".join(evb) + b"".join(waves) + bytes(vag))
    json.dump(dict(map=map_id, rate=rate, models=models, missing_banks=missing, banks=[b.name for b, _ in banks],
                   samples=names, events=ev_json, stale_wave_refs=stale),
              open(os.path.join(out, f"snd_{map_id}.json"), "w"), indent=1)
    row.update(file=os.path.getsize(path), events=len(evb), stale=stale, snr=float(np.median(snr)) if snr else 0,
               secs=time.time() - t0)
    return row


def main():
    a = sys.argv[1:]
    out = a.pop(0)
    size_only = "--size-only" in a
    a = [x for x in a if x != "--size-only"]
    game = gamepath.game_dir()
    snd = os.path.join(game, "sound")
    maps = a or sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(game, "map/MapStudio/m1*.msb")))
    os.makedirs(out, exist_ok=True)
    print(f"SPU2 budget for resident samples: {SPU_BUDGET} B (2 MB - system {SPU_SYSTEM:#x} - reverb {SPU_REVERB:#x} "
          f"- stream buffers {SPU_STREAM:#x})")
    print(f"tiers (MB at the chosen rate): core resident | all character banks (largest {CHR_RESIDENT} resident) | "
          f"on-demand spell/SFX (pool {DEMAND_POOL // 1024} KB) | streamed > {STREAM_SECONDS:g} s")
    print(f"{'map':14} {'chr':>3} {'smp':>4} " + " ".join(f"{'need@' + str(r // 1000) + 'k':>9}" for r in RATES) +
          f" {'rate':>5} {'core':>5} {'chr':>5} {'chrmax':>6} {'demand':>6} {'strm':>5} {'cut max':>7}"
          f" {'sm/xm MB':>8} {'min':>5}" + ("" if size_only else f" {'events':>6} {'file MB':>7} {'SNR':>5} {'s':>4}"))
    for m in maps:
        r = export(out, game, snd, m, size_only)
        T = r["tiers"][r["rate"]]
        line = (f"{m:14} {r['models']:3} {r['samples']:4} " +
                " ".join(f"{r['need'][x] / 1048576:9.2f}" for x in RATES) +
                f" {r['rate']:5} {T[0] / 1048576:5.2f} {T[1] / 1048576:5.2f} {r['chr_max'] / 1048576:6.2f}"
                f" {T[2] / 1048576:6.2f} {T[3] / 1048576:5.2f} {r['cutscene_max'] / 1024:6.0f}K"
                f" {r['streamed'] / 1048576:8.1f} {r['streamed_min']:5.0f}")
        if not size_only:
            line += f" {r['events']:6} {r['file'] / 1048576:7.2f} {r['snr']:5.1f} {r['secs']:4.0f}"
        if r["need"][r["rate"]] > SPU_BUDGET:
            line += "  DOES NOT FIT"
        if r["missing"]:
            line += f"  no bank: {' '.join(r['missing'])}"
        print(line, flush=True)


if __name__ == "__main__":
    main()
