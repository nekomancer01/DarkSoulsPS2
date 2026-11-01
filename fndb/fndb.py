#!/usr/bin/env python3
"""DSR function database: import Ghidra exports, rank by porting priority, label with a local LLM.

Subcommands:
  import-meta FILE.jsonl     functions + call graph from ExportFunctions.java
  prioritize                 assign tier/score/subsystem hint
  select --tier T --limit N  print the next entry addresses that need decompiling
  import-decomp FILE.jsonl   decompiled C from DecompileList.java
  label --limit N            label decompiled functions with Ollama, throttled
  embed                      vectorize new/changed labels (needs ~/osrs/.venv-wiki/bin/python)
  search "query"             hybrid vector + keyword lookup over labels
  status                     counts per tier
"""
import argparse
import json
import os
import re
import sqlite3
import subprocess
import sys
import time
import urllib.request
from collections import defaultdict, deque

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.environ.get("FNDB_DB", os.path.join(HERE, "dsr-functions.sqlite"))

SCHEMA = """
CREATE TABLE IF NOT EXISTS functions (
  entry TEXT PRIMARY KEY, name TEXT, size INTEGER, block TEXT, is_thunk INTEGER, n_callers INTEGER,
  strings TEXT, tier INTEGER, score REAL, hint TEXT,
  decomp TEXT, decomp_at TEXT, ctx INTEGER,
  label_name TEXT, summary TEXT, subsystem TEXT, ps2 TEXT, confidence REAL, model TEXT, labeled_at TEXT,
  ps2_impl TEXT, notes TEXT
);
CREATE TABLE IF NOT EXISTS calls (caller TEXT, callee TEXT, PRIMARY KEY (caller, callee));
CREATE INDEX IF NOT EXISTS calls_callee ON calls(callee);
CREATE INDEX IF NOT EXISTS fn_tier ON functions(tier, score);
"""

# Subsystem keyword seeds, matched against referenced strings and callee names.
# weight: how much porting the subsystem needs the original logic (render/net/sound get replaced on PS2).
SUBSYSTEMS = {
    "file":   (10, r"\.dcx|\.bnd|\.bhd|\.bdt|DCX|BND3|BHD5|\.tpf|\.flver|\.msb|\.luabnd|\.emevd|CreateFile|ReadFile|dvdbnd|archive"),
    "param":  (10, r"Param|PARAM|paramdef|\.param"),
    "event":  (9,  r"emevd|EMEVD|EventFlag|eventflag"),
    "ai":     (8,  r"\blua|Lua|luabnd|Esd|ESD|AiThink|Logic|Goal"),
    "anim":   (7,  r"\.tae|TAE|\.hkx|hka|Havok|hkb|Anim"),
    "map":    (8,  r"MapStudio|\bm1[0-9]_|MSB|MapPiece|Collision|Navmesh"),
    "chr":    (8,  r"ChrIns|PlayerIns|EnemyIns|\bc[0-9]{4}\b|Chr"),
    "combat": (9,  r"Damage|Attack|Guard|Stamina|Poise|SpEffect|Bullet|Hit"),
    "save":   (6,  r"\.sl2|DRAKS|SaveData|Profile"),
    "ui":     (3,  r"Menu|\.fmg|Dialog|Font|Text"),
    "render": (1,  r"D3D11|DXGI|Shader|\.hlsl|RenderTarget|GX|Texture"),
    "sound":  (1,  r"FMOD|\.fsb|\.fev|Sound"),
    "net":    (0,  r"Steam|Session|Lobby|Matchmaking|WSA|socket|BloodMessage|Ghost"),
}
SKIP_NAME = re.compile(r"^(_|__|operator|std::|Concurrency::|`|memcpy|memset|strlen|wcs|str|malloc|free|thunk_)")


# Context buckets (num_ctx) -> parallel slots. KV cache = ctx x slots, so each bucket stays fully on an 8 GB card.
BUCKETS = {4096: 4, 8192: 2, 16384: 1}
PROMPT_OVERHEAD_TOKENS = 400 + 700 + 500  # output budget + system prompt + strings/callee list
CHARS_PER_TOKEN = 1.7  # measured ~1.8 for Ghidra C on qwen2.5 (16571 chars -> 9027 tokens), with margin


def fit_chars(ctx, cpt=CHARS_PER_TOKEN):
    """Longest decompiled C (in chars) whose prompt fits in ctx."""
    return int((ctx - PROMPT_OVERHEAD_TOKENS) * cpt)


def ctx_for(chars, cpt=CHARS_PER_TOKEN):
    """Smallest bucket that fits; the largest bucket takes everything else (truncated)."""
    for ctx in sorted(BUCKETS):
        if chars <= fit_chars(ctx, cpt):
            return ctx
    return max(BUCKETS)


def db():
    con = sqlite3.connect(DB)
    con.executescript(SCHEMA)
    cols = {r[1] for r in con.execute("PRAGMA table_info(functions)")}
    if "ctx" not in cols:
        con.execute("ALTER TABLE functions ADD COLUMN ctx INTEGER")
    con.create_function("ctx_for", 1, lambda n: ctx_for(n or 0))
    return con


def cmd_import_meta(a):
    con = db()
    n = 0
    with open(a.file, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            con.execute("""INSERT INTO functions(entry,name,size,block,is_thunk,n_callers,strings)
                           VALUES(?,?,?,?,?,?,?)
                           ON CONFLICT(entry) DO UPDATE SET name=excluded.name,size=excluded.size,block=excluded.block,
                           is_thunk=excluded.is_thunk,n_callers=excluded.n_callers,strings=excluded.strings""",
                        (r["entry"], r["name"], r["size"], r["block"], int(r["thunk"]), r["n_callers"],
                         json.dumps(r["strings"])))
            con.executemany("INSERT OR IGNORE INTO calls VALUES(?,?)", [(r["entry"], c[0]) for c in r["callees"]])
            n += 1
    con.commit()
    print(f"imported {n} functions")


def cmd_prioritize(a):
    con = db()
    rows = con.execute("SELECT entry,name,size,block,is_thunk,strings FROM functions").fetchall()
    names = {e: n for e, n, *_ in rows}
    callees = defaultdict(list)
    callers = defaultdict(list)
    for c, d in con.execute("SELECT caller,callee FROM calls"):
        callees[c].append(d)
        callers[d].append(c)
    blocks = sorted({r[3] for r in rows if r[3].startswith(".text")})
    second_text = blocks[1] if len(blocks) > 1 else None

    seed = {}
    for entry, name, size, block, thunk, strings in rows:
        text = " ".join(json.loads(strings)) + " " + " ".join(names.get(c, "") for c in callees[entry])
        best = None
        for sub, (w, pat) in SUBSYSTEMS.items():
            hits = len(re.findall(pat, text))
            if hits and (best is None or w * hits > best[1]):
                best = (sub, w * min(hits, 5))
        if best:
            seed[entry] = best

    # Spread seed weight two call hops in both directions, halving per hop.
    score = defaultdict(float)
    hint = {}
    for entry, (sub, w) in seed.items():
        q = deque([(entry, 0)])
        seen = {entry}
        while q:
            e, d = q.popleft()
            s = w * (0.5 ** d)
            if s > score[e]:
                score[e] = s
                hint[e] = sub
            if d < 2:
                for nb in callees[e] + callers[e]:
                    if nb not in seen:
                        seen.add(nb)
                        q.append((nb, d + 1))

    tiers = defaultdict(int)
    for entry, name, size, block, thunk, strings in rows:
        s, h = score.get(entry, 0.0), hint.get(entry)
        if thunk or size < 16 or (not name.startswith("FUN_") and SKIP_NAME.match(name)):
            t = 9  # thunks, tiny stubs, CRT/STL already named by Ghidra
        elif block == second_text:
            t = 5  # second .text: likely packer/obfuscation
        elif h in ("render", "sound", "net"):
            t = 4  # replaced on PS2; label late
        elif s >= 8:
            t = 1
        elif s >= 3:
            t = 2
        else:
            t = 3
        tiers[t] += 1
        con.execute("UPDATE functions SET tier=?,score=?,hint=? WHERE entry=?", (t, s, h, entry))
    con.commit()
    print("tiers:", dict(sorted(tiers.items())))


def cmd_select(a):
    con = db()
    for (e,) in con.execute("""SELECT entry FROM functions WHERE tier<=? AND decomp IS NULL
                               ORDER BY tier, score DESC, size LIMIT ?""", (a.tier, a.limit)):
        print(e)


def cmd_import_decomp(a):
    con = db()
    n = 0
    with open(a.file, encoding="utf-8") as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                print("skipping incomplete line (decompile was interrupted)", file=sys.stderr)
                continue
            con.execute("UPDATE functions SET decomp=?, decomp_at=datetime('now'), ctx=? WHERE entry=?",
                        (r["c"], ctx_for(len(r["c"])), r["entry"]))
            n += 1
    con.commit()
    print(f"imported {n} decompiled functions")


def cmd_assign_ctx(a):
    """Recompute every decompiled function's context bucket (e.g. after changing BUCKETS)."""
    con = db()
    con.execute("UPDATE functions SET ctx=ctx_for(length(decomp)) WHERE decomp IS NOT NULL")
    con.commit()
    for ctx, n in con.execute("SELECT ctx, COUNT(*) FROM functions WHERE decomp IS NOT NULL GROUP BY ctx"):
        print(f"ctx {ctx}: {n}")


# ---- labeling -------------------------------------------------------------------------------------------

LABEL_SCHEMA = {
    "type": "object",
    "properties": {
        "name": {"type": "string"},
        "summary": {"type": "string"},
        "subsystem": {"type": "string", "enum": list(SUBSYSTEMS) + ["math", "memory", "container", "string", "util", "unknown"]},
        "ps2": {"type": "string", "enum": ["port", "adapt", "replace", "drop"]},
        "confidence": {"type": "number"},
    },
    "required": ["name", "summary", "subsystem", "ps2", "confidence"],
}
SYSTEM = """You label decompiled functions from a 64-bit Windows game engine (Dark Souls Remastered) for a PS2 reimplementation.
Reply with JSON only:
- name: a short CamelCase name for what the function does (guess if needed, e.g. LoadParamFile, ChrIns_ApplyDamage)
- summary: one sentence, concrete (inputs, outputs, side effects)
- subsystem: closest category
- ps2: port = game logic to reimplement as-is; adapt = needs changes for PS2 limits; replace = platform/middleware code (D3D, FMOD, Havok, Steam, Win32) replaced by PS2 code; drop = not needed (debug, online, telemetry)
- confidence: 0.0-1.0 for your name/summary
Base everything on the code shown. Names of called functions in comments are earlier labels and may be wrong."""


def gpu_busy(max_util):
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=10).stdout
        return int(out.split()[0]) > max_util
    except Exception:
        return False


def blocked(a):
    """Reason to wait before the next request, or None. All checks are off by default."""
    load = os.getloadavg()[0]
    if load > a.max_load:
        return f"load {load:.1f} > {a.max_load}"
    for pat in a.pause_if:
        if subprocess.run(["pgrep", "-f", pat], capture_output=True).returncode == 0:
            return f"process matching {pat!r} running"
    if a.max_gpu <= 100 and gpu_busy(a.max_gpu):  # includes our own load
        return f"GPU utilization > {a.max_gpu}%"
    return None


def ollama(a, ctx, messages):
    req = {
        "model": a.model, "messages": messages, "stream": False, "format": LABEL_SCHEMA,
        "keep_alive": "5m",
        "options": {"temperature": 0.1, "num_ctx": ctx, "num_thread": a.threads, "num_predict": 400},
    }
    if a.model.startswith(("qwen3", "deepseek-r1")):
        req["think"] = False  # reasoning models: thinking tokens would dominate decode time
    body = json.dumps(req).encode()
    req = urllib.request.Request(a.host + "/api/chat", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        resp = json.load(r)
    return json.loads(resp["message"]["content"]), resp


def build_prompt(con, row, max_chars):
    entry, name, size, strings, decomp = row
    labels = con.execute("""SELECT f.entry, COALESCE(f.label_name, f.name), f.summary FROM calls c
                            JOIN functions f ON f.entry=c.callee WHERE c.caller=? LIMIT 40""", (entry,)).fetchall()
    callee_txt = "\n".join(f"- {e} {n}: {s or '(unlabeled)'}" for e, n, s in labels) or "- none"
    strs = json.loads(strings or "[]")
    c = decomp if len(decomp) <= max_chars else decomp[:max_chars] + "\n// [truncated]"
    return (f"Function {name} @ {entry}, {size} bytes.\n"
            f"Strings referenced: {json.dumps(strs[:24])}\n"
            f"Called functions:\n{callee_txt}\n\nDecompiled C:\n{c}")


RATES = os.path.join(HERE, "rates.json")
# Wall seconds per labeled function with all slots busy (qwen2.5-coder:7b, 3070, synthetic test 2026-10-30).
# Overwritten by each finished bucket so later estimates use measured speed.
DEFAULT_RATES = {4096: 1.2, 8192: 3.5, 16384: 7.5}


def load_rates(model):
    try:
        with open(RATES) as f:
            saved = json.load(f).get(model, {})
    except (OSError, ValueError):
        saved = {}
    return {ctx: float(saved.get(str(ctx), DEFAULT_RATES.get(ctx, 5.0))) for ctx in BUCKETS}


def save_rate(model, ctx, rate):
    try:
        with open(RATES) as f:
            data = json.load(f)
    except (OSError, ValueError):
        data = {}
    data.setdefault(model, {})[str(ctx)] = round(rate, 3)
    with open(RATES, "w") as f:
        json.dump(data, f, indent=1)


def hms(sec):
    sec = int(max(sec, 0))
    return f"{sec // 3600}:{sec // 60 % 60:02d}:{sec % 60:02d}"


def cmd_label(a):
    """Label everything decompiled in tiers <= --tier, one context bucket at a time (smallest first).

    num_ctx is fixed per model load and KV memory is ctx x slots, so each bucket restarts Ollama with its own
    slot count and keeps every slot busy until the bucket is empty, then switches to the next bucket.
    """
    from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
    import signal
    # Every label is committed as it arrives, so a rerun with the same command continues where this one stopped.
    # First Ctrl+C / window close: stop sending, save the requests in flight. Second: quit now (those few are redone).
    stopping = []

    def on_signal(sig, frame):
        if stopping:
            print("\nquitting now; in-flight functions will be redone next run", file=sys.stderr, flush=True)
            os._exit(130)
        stopping.append(sig)
        print("\nstopping after the requests in flight (Ctrl+C again to quit now)", file=sys.stderr, flush=True)

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, on_signal)
    con = db()
    con.execute("UPDATE functions SET ctx=ctx_for(length(decomp)) WHERE decomp IS NOT NULL AND ctx IS NULL")
    con.commit()
    buckets = sorted(BUCKETS) if a.ctx is None else [a.ctx]
    started = time.time()
    done = 0
    todo = dict(con.execute(f"""
        SELECT ctx, COUNT(*) FROM functions
        WHERE decomp IS NOT NULL AND labeled_at IS NULL AND tier<=? AND decomp NOT LIKE '// %'
          AND ctx IN ({",".join(map(str, buckets))}) GROUP BY ctx""", (a.tier,)).fetchall())
    total = min(sum(todo.values()), a.limit)
    rates = load_rates(a.model)
    plan = ", ".join(f"ctx {c}: {todo.get(c, 0)} (~{hms(todo.get(c, 0) * rates[c])})" for c in buckets)
    print(f"{total} functions to label | {plan} | estimated total {hms(sum(todo.get(c, 0) * rates[c] for c in buckets))}",
          flush=True)
    out_of_time = lambda: a.max_minutes > 0 and time.time() - started > a.max_minutes * 60
    for i, ctx in enumerate(buckets):
        if done >= a.limit or out_of_time() or stopping:
            break
        # Bottom-up order, fixed per bucket: callees with fewer unlabeled callees first. Prompts are built when
        # submitted, so they include every label finished up to that moment.
        rows = con.execute("""
            SELECT f.entry, f.name, f.size, f.strings, f.decomp FROM functions f
            WHERE f.decomp IS NOT NULL AND f.labeled_at IS NULL AND f.tier<=? AND f.decomp NOT LIKE '// %' AND f.ctx=?
            ORDER BY
              (SELECT COUNT(*) FROM calls c JOIN functions g ON g.entry=c.callee
               WHERE c.caller=f.entry AND g.labeled_at IS NULL AND g.tier<9),
              f.tier, f.score DESC
            LIMIT ?""", (a.tier, ctx, a.limit - done)).fetchall()
        if not rows:
            print(f"ctx {ctx}: nothing to label")
            continue
        slots = a.workers or BUCKETS.get(ctx, 1)
        if not a.no_server:
            subprocess.run([os.path.join(HERE, "ollama-up.sh"), str(slots)], check=True)
        print(f"ctx {ctx}: {len(rows)} functions, {slots} parallel", flush=True)
        max_chars = fit_chars(ctx, a.chars_per_token)
        next_ctx = next((b for b in sorted(BUCKETS) if b > ctx), None)
        t_bucket = time.time()
        n_bucket = 0
        pending = deque(rows)
        inflight = {}
        with ThreadPoolExecutor(slots) as pool:
            while pending or inflight:
                while pending and len(inflight) < slots and not out_of_time() and not stopping:
                    while (why := blocked(a)) and not stopping:
                        print(f"waiting: {why}", flush=True)
                        time.sleep(a.wait)
                    if stopping:
                        break
                    row = pending.popleft()
                    msgs = [{"role": "system", "content": SYSTEM},
                            {"role": "user", "content": build_prompt(con, row, max_chars)}]
                    inflight[pool.submit(ollama, a, ctx, msgs)] = row
                if not inflight:
                    break
                finished, _ = wait(inflight, return_when=FIRST_COMPLETED)
                for fut in finished:
                    row = inflight.pop(fut)
                    try:
                        out, resp = fut.result()
                    except Exception as e:
                        print(f"{row[0]}: request failed: {e}", file=sys.stderr)
                        continue
                    n_in = resp.get("prompt_eval_count") or 0
                    # An over-long prompt is silently cut to ~ctx/2 tokens from the front, dropping the system
                    # prompt: discard the label and move the function up a bucket.
                    if ctx // 2 <= n_in <= ctx // 2 + 8:
                        if next_ctx:
                            con.execute("UPDATE functions SET ctx=? WHERE entry=?", (next_ctx, row[0]))
                            con.commit()
                        print(f"{row[0]}: prompt truncated at ctx {ctx}; moved to ctx {next_ctx}", file=sys.stderr)
                        continue
                    con.execute("""UPDATE functions SET label_name=?, summary=?, subsystem=?, ps2=?, confidence=?,
                                   model=?, labeled_at=datetime('now') WHERE entry=?""",
                                (out["name"], out["summary"], out["subsystem"], out["ps2"], float(out["confidence"]),
                                 a.model, row[0]))
                    con.commit()
                    done += 1
                    n_bucket += 1
                    # Measured rate once a few have finished (the first ones include model load), else the prior.
                    rate = (time.time() - t_bucket) / n_bucket if n_bucket >= 4 else rates[ctx]
                    left_bucket = (len(rows) - n_bucket) * rate
                    left_later = sum(todo.get(c, 0) * rates[c] for c in buckets[i + 1:])
                    print(f"[{done}/{total} {100 * done / max(total, 1):5.1f}%] ctx {ctx} {n_bucket}/{len(rows)} "
                          f"{rate:.2f}s/fn | bucket ETA {hms(left_bucket)} | total ETA {hms(left_bucket + left_later)}"
                          f" | {row[0]} -> {out['name']} ({out['subsystem']}/{out['ps2']}, {out['confidence']:.2f})",
                          flush=True)
        el = time.time() - t_bucket
        print(f"ctx {ctx}: labeled {n_bucket} in {hms(el)} ({el / max(n_bucket, 1):.2f}s each)", flush=True)
        if n_bucket >= 4:
            save_rate(a.model, ctx, el / n_bucket)
    if not a.no_server:
        subprocess.run([os.path.join(HERE, "ollama-up.sh"), str(BUCKETS[min(BUCKETS)])], check=False)
    print(f"labeled {done}/{total} in {hms(time.time() - started)}")
    if stopping or done < total:
        print("not finished; run the same command again to continue")


# ---- vector search --------------------------------------------------------------------------------------
# Same stack as the OSRS wiki search (~/osrs-wiki-backup/embed_db.py): bge-small via fastembed on CUDA, sqlite-vec,
# in a separate file so the main DB is never locked. Needs ~/osrs/.venv-wiki/bin/python for embed/search.
#   fn_vec(embedding float[384])  rowid = fn_map.rowid        fn_fts(entry, text)  for exact names/strings
VEC_DB = os.environ.get("FNDB_VEC_DB", os.path.splitext(DB)[0] + "-vec.sqlite")
EMBED_MODEL = "BAAI/bge-small-en-v1.5"


def vec_db():
    import sqlite_vec
    c = sqlite3.connect(VEC_DB)
    c.enable_load_extension(True)
    sqlite_vec.load(c)
    c.executescript("""
        CREATE VIRTUAL TABLE IF NOT EXISTS fn_vec USING vec0(embedding float[384]);
        CREATE TABLE IF NOT EXISTS fn_map (rowid INTEGER PRIMARY KEY, entry TEXT UNIQUE, labeled_at TEXT);
        CREATE VIRTUAL TABLE IF NOT EXISTS fn_fts USING fts5(entry UNINDEXED, text);
    """)
    return c


def embedder(cpu):
    import onnxruntime
    from fastembed import TextEmbedding
    if not cpu:
        onnxruntime.preload_dlls()
    return TextEmbedding(EMBED_MODEL, providers=["CPUExecutionProvider"] if cpu else ["CUDAExecutionProvider"])


def embed_text(label, name, summary, subsystem, ps2, strings):
    strs = ", ".join(json.loads(strings or "[]")[:12])
    return f"{label} ({name}) [{subsystem}/{ps2}]: {summary}" + (f"\nstrings: {strs}" if strs else "")


def cmd_embed(a):
    """Embed labeled functions that are new or relabeled since the last run; resumable, commits per batch."""
    import sqlite_vec
    con, v = db(), vec_db()
    seen = dict(v.execute("SELECT entry, labeled_at FROM fn_map"))
    rows = [r for r in con.execute("""SELECT entry, label_name, name, summary, subsystem, ps2, strings, labeled_at
                                      FROM functions WHERE labeled_at IS NOT NULL""")
            if seen.get(r[0]) != r[7]]
    if not rows:
        print("embeddings up to date")
        return
    model = embedder(a.cpu)
    t0 = time.time()
    for i in range(0, len(rows), 256):
        chunk = rows[i:i + 256]
        texts = [embed_text(*r[1:7]) for r in chunk]
        for r, text, vec in zip(chunk, texts, model.embed(texts, batch_size=256)):
            v.execute("INSERT OR IGNORE INTO fn_map(entry) VALUES (?)", (r[0],))
            rid = v.execute("SELECT rowid FROM fn_map WHERE entry=?", (r[0],)).fetchone()[0]
            v.execute("DELETE FROM fn_vec WHERE rowid=?", (rid,))
            v.execute("INSERT INTO fn_vec(rowid, embedding) VALUES (?, ?)", (rid, sqlite_vec.serialize_float32(vec.tolist())))
            v.execute("DELETE FROM fn_fts WHERE entry=?", (r[0],))
            v.execute("INSERT INTO fn_fts(entry, text) VALUES (?, ?)", (r[0], text))
            v.execute("UPDATE fn_map SET labeled_at=? WHERE rowid=?", (r[7], rid))
        v.commit()
        print(f"embedded {i + len(chunk)}/{len(rows)}", flush=True)
    print(f"embedded {len(rows)} in {time.time() - t0:.1f}s")


def cmd_search(a):
    """Hybrid search: vector similarity + FTS5 keywords, merged by reciprocal rank."""
    import sqlite_vec
    con, v = db(), vec_db()
    model = embedder(a.cpu)
    qv = next(iter(model.query_embed(a.query)))
    pool = a.k * 4
    vec_hits = [e for e, in v.execute("""SELECT m.entry FROM fn_vec JOIN fn_map m ON m.rowid=fn_vec.rowid
                                         WHERE embedding MATCH ? AND k = ? ORDER BY distance""",
                                      (sqlite_vec.serialize_float32(qv.tolist()), pool))]
    terms = " OR ".join('"' + t.replace('"', '') + '"' for t in re.findall(r"\w+", a.query))
    fts_hits = [e for e, in v.execute("SELECT entry FROM fn_fts WHERE fn_fts MATCH ? ORDER BY rank LIMIT ?",
                                      (terms, pool))] if terms else []
    score = defaultdict(float)
    for hits in (vec_hits, fts_hits):
        for rank, e in enumerate(hits):
            score[e] += 1 / (60 + rank)
    shown = 0
    for e in sorted(score, key=score.get, reverse=True):
        r = con.execute("SELECT entry, label_name, subsystem, ps2, tier, confidence, summary FROM functions WHERE entry=?",
                        (e,)).fetchone()
        if r is None or (a.subsystem and r[2] != a.subsystem) or (a.tier and r[4] > a.tier):
            continue
        print(f"{r[0]}  {r[1]:<28} {r[2]}/{r[3]:<7} tier {r[4]}  conf {r[5]:.2f}  {r[6]}")
        shown += 1
        if shown >= a.k:
            break
    if not shown:
        print("no matches (run `embed` after labeling)")


# Code index: embeddings of the decompiled C itself (no labels needed), plus FTS over the code.
CODE_DB = os.environ.get("FNDB_CODE_DB", os.path.splitext(DB)[0] + "-code.sqlite")
CODE_MODEL = "jinaai/jina-embeddings-v2-base-code"
JP_DB = os.path.join(HERE, "..", "jp", "jp-strings.sqlite")


def code_db():
    import sqlite_vec
    c = sqlite3.connect(CODE_DB)
    c.enable_load_extension(True)
    sqlite_vec.load(c)
    c.executescript("""
        CREATE VIRTUAL TABLE IF NOT EXISTS code_vec USING vec0(embedding float[768]);
        CREATE TABLE IF NOT EXISTS code_map (rowid INTEGER PRIMARY KEY, entry TEXT UNIQUE, decomp_at TEXT);
        CREATE VIRTUAL TABLE IF NOT EXISTS code_fts USING fts5(entry UNINDEXED, text);
    """)
    return c


def jp_english():
    """Japanese string -> English from ../jp/translate.py, so code that prints Japanese is findable in English."""
    if not os.path.exists(JP_DB):
        return {}
    return dict(sqlite3.connect(JP_DB).execute("SELECT s, en FROM strings WHERE en IS NOT NULL"))


def code_text(name, strings, c, en, max_chars):
    strs = json.loads(strings or "[]")
    tr = [f"{s} = {en[s]}" for s in strs if s in en]
    head = f"// {name}\n" + (f"// strings: {', '.join(strs[:12])}\n" if strs else "") + \
        (f"// english: {'; '.join(tr[:12])}\n" if tr else "")
    return head + c[:max_chars]


def cmd_embed_code(a):
    """Embed decompiled functions that are new or re-decompiled since the last run; resumable, commits per batch."""
    import sqlite_vec
    con, v = db(), code_db()
    seen = dict(v.execute("SELECT entry, decomp_at FROM code_map"))
    rows = [r for r in con.execute("""SELECT entry, name, strings, decomp, decomp_at FROM functions
                                      WHERE decomp IS NOT NULL AND decomp NOT LIKE '// %'""")
            if seen.get(r[0]) != r[4]]
    if not rows:
        print("code embeddings up to date")
        return
    en = jp_english()
    import onnxruntime
    from fastembed import TextEmbedding
    if not a.cpu:
        onnxruntime.preload_dlls()
    model = TextEmbedding(CODE_MODEL, max_length=a.max_tokens,
                          providers=["CPUExecutionProvider"] if a.cpu else ["CUDAExecutionProvider"])
    # Short functions first: similar lengths batch with little padding, and progress shows early.
    rows.sort(key=lambda r: len(r[3]))
    t0, step = time.time(), 64
    for i in range(0, len(rows), step):
        chunk = rows[i:i + step]
        texts = [code_text(r[1], r[2], r[3], en, a.max_tokens * 3) for r in chunk]
        # Attention memory grows with length squared (~200 MB per 2048-token input), so shrink the batch by the
        # square of the length; ~2 chars per token.
        tokens = min(a.max_tokens, max(len(t) for t in texts) // 2 + 1)
        bs = max(1, int(a.batch * (512 / tokens) ** 2))
        vecs = list(model.embed(texts, batch_size=bs))
        if len(vecs) != len(chunk):
            sys.exit(f"embedder returned {len(vecs)} vectors for {len(chunk)} texts")
        for r, text, vec in zip(chunk, texts, vecs):
            v.execute("INSERT OR IGNORE INTO code_map(entry) VALUES (?)", (r[0],))
            rid = v.execute("SELECT rowid FROM code_map WHERE entry=?", (r[0],)).fetchone()[0]
            v.execute("DELETE FROM code_vec WHERE rowid=?", (rid,))
            v.execute("INSERT INTO code_vec(rowid, embedding) VALUES (?, ?)",
                      (rid, sqlite_vec.serialize_float32(vec.tolist())))
            v.execute("DELETE FROM code_fts WHERE entry=?", (r[0],))
            v.execute("INSERT INTO code_fts(entry, text) VALUES (?, ?)", (r[0], text))
            v.execute("UPDATE code_map SET decomp_at=? WHERE rowid=?", (r[4], rid))
        v.commit()
        done = i + len(chunk)
        if done % 1024 < step or done == len(rows):
            el = time.time() - t0
            print(f"embedded {done}/{len(rows)} ({100 * done / len(rows):.1f}%), {el:.0f}s", flush=True)
    print(f"embedded {len(rows)} in {time.time() - t0:.1f}s")


def cmd_code_search(a):
    """Find functions by describing them (vector + FTS keywords over the C), or functions similar to --like ENTRY."""
    import sqlite_vec
    con, v = db(), code_db()
    pool = a.k * 20  # wide pool: the size and tier filters drop many hits
    if a.like:
        rid = v.execute("SELECT rowid FROM code_map WHERE entry=?", (a.like,)).fetchone()
        if not rid:
            sys.exit(f"{a.like} has no code embedding")
        qv = v.execute("SELECT embedding FROM code_vec WHERE rowid=?", rid).fetchone()[0]
        fts_hits = []
    else:
        import onnxruntime
        from fastembed import TextEmbedding
        if a.gpu:
            onnxruntime.preload_dlls()
        model = TextEmbedding(CODE_MODEL, providers=["CUDAExecutionProvider"] if a.gpu else ["CPUExecutionProvider"])
        qv = sqlite_vec.serialize_float32(next(iter(model.query_embed(a.query))).tolist())
        terms = " OR ".join('"' + t.replace('"', '') + '"' for t in re.findall(r"\w+", a.query))
        fts_hits = [e for e, in v.execute("SELECT entry FROM code_fts WHERE code_fts MATCH ? ORDER BY rank LIMIT ?",
                                          (terms, pool))] if terms else []
    vec_hits = [e for e, in v.execute("""SELECT m.entry FROM code_vec JOIN code_map m ON m.rowid=code_vec.rowid
                                         WHERE embedding MATCH ? AND k = ? ORDER BY distance""", (qv, pool))]
    score = defaultdict(float)
    for hits in (vec_hits, fts_hits):
        for rank, e in enumerate(hits):
            score[e] += 1 / (60 + rank)
    shown = 0
    for e in sorted(score, key=score.get, reverse=True):
        if e == a.like:
            continue
        r = con.execute("SELECT entry, name, size, tier, n_callers, strings FROM functions WHERE entry=?",
                        (e,)).fetchone()
        if r is None or (a.tier and r[3] > a.tier) or r[2] < a.min_size:
            continue
        strs = ", ".join(json.loads(r[5] or "[]")[:3])
        print(f"{r[0]}  {r[1][:40]:<40} {r[2]:>6}B  tier {r[3]}  callers {r[4]:<4} {strs[:70]}")
        shown += 1
        if shown >= a.k:
            break
    if not shown:
        print("no matches (run `embed-code` after decompiling)")


def cmd_import_jp_refs(a):
    """Add Japanese strings found by ghidra/scripts/JpRefs.java to each function's strings, and drop those functions
    from the code index so the next `embed-code` re-embeds them with their English."""
    con = db()
    add = defaultdict(list)
    for line in open(a.file, encoding="utf-8"):
        entry, _, text = line.rstrip("\n").partition("\t")
        if text:
            add[entry].append(text)
    changed = []
    for entry, new in add.items():
        r = con.execute("SELECT strings FROM functions WHERE entry=?", (entry,)).fetchone()
        if r is None:
            continue
        old = json.loads(r[0] or "[]")
        merged = old + [t for t in dict.fromkeys(new) if t not in old]
        if len(merged) != len(old):
            con.execute("UPDATE functions SET strings=? WHERE entry=?", (json.dumps(merged, ensure_ascii=False), entry))
            changed.append(entry)
    con.commit()
    if os.path.exists(CODE_DB):
        v = code_db()
        for e in add:  # all of them, not only `changed`: their English may have been re-translated
            rid = v.execute("SELECT rowid FROM code_map WHERE entry=?", (e,)).fetchone()
            if rid:
                v.execute("DELETE FROM code_vec WHERE rowid=?", rid)
                v.execute("DELETE FROM code_fts WHERE entry=?", (e,))
                v.execute("DELETE FROM code_map WHERE rowid=?", rid)
        v.commit()
    print(f"{len(changed)} functions got new Japanese strings; {len(add)} queued for embed-code")


def cmd_status(a):
    con = db()
    print(f"{'tier':>4} {'total':>7} {'decomp':>7} {'labeled':>7}   decompiled by ctx bucket")
    for t, n, d, l in con.execute("""SELECT tier, COUNT(*), COUNT(decomp), COUNT(labeled_at) FROM functions
                                     GROUP BY tier ORDER BY tier"""):
        by_ctx = ", ".join(f"{c}: {k}" for c, k in con.execute(
            "SELECT ctx, COUNT(*) FROM functions WHERE tier IS ? AND decomp IS NOT NULL GROUP BY ctx", (t,)))
        print(f"{t!s:>4} {n:>7} {d:>7} {l:>7}   {by_ctx}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("import-meta"); s.add_argument("file"); s.set_defaults(fn=cmd_import_meta)
    s = sp.add_parser("prioritize"); s.set_defaults(fn=cmd_prioritize)
    s = sp.add_parser("select"); s.add_argument("--tier", type=int, default=2)
    s.add_argument("--limit", type=int, default=1_000_000); s.set_defaults(fn=cmd_select)
    s = sp.add_parser("import-decomp"); s.add_argument("file"); s.set_defaults(fn=cmd_import_decomp)
    s = sp.add_parser("assign-ctx"); s.set_defaults(fn=cmd_assign_ctx)
    s = sp.add_parser("label")
    s.add_argument("--tier", type=int, default=2, help="label tiers <= this")
    s.add_argument("--limit", type=int, default=1_000_000)
    s.add_argument("--max-minutes", type=float, default=0, help="stop after this long (0 = no limit)")
    s.add_argument("--model", default=os.environ.get("FNDB_MODEL", "qwen2.5-coder:7b"))
    s.add_argument("--host", default=os.environ.get("OLLAMA_HOST_URL", "http://127.0.0.1:11434"))
    s.add_argument("--ctx", type=int, choices=sorted(BUCKETS), help="only this bucket (default: all, smallest first)")
    s.add_argument("--workers", type=int, help="parallel requests (default: per bucket, see BUCKETS)")
    s.add_argument("--no-server", action="store_true", help="don't (re)start Ollama per bucket")
    s.add_argument("--threads", type=int, default=4, help="CPU threads for layers that spill out of VRAM")
    s.add_argument("--chars-per-token", type=float, default=CHARS_PER_TOKEN, help="estimate for decompiled C")
    # Throttles are off by default; set them to leave the PC usable while labeling.
    s.add_argument("--max-load", type=float, default=1e9, help="wait while 1-min load average is above this")
    s.add_argument("--max-gpu", type=int, default=101, help="wait while GPU utilization is above this %%")
    s.add_argument("--pause-if", nargs="*", default=[], help="wait while processes matching these run")
    s.add_argument("--wait", type=float, default=30, help="seconds between busy checks")
    s.set_defaults(fn=cmd_label)
    s = sp.add_parser("embed"); s.add_argument("--cpu", action="store_true"); s.set_defaults(fn=cmd_embed)
    s = sp.add_parser("search"); s.add_argument("query"); s.add_argument("-k", type=int, default=10)
    s.add_argument("--subsystem"); s.add_argument("--tier", type=int); s.add_argument("--cpu", action="store_true")
    s.set_defaults(fn=cmd_search)
    s = sp.add_parser("embed-code"); s.add_argument("--cpu", action="store_true")
    s.add_argument("--max-tokens", type=int, default=2048); s.add_argument("--batch", type=int, default=16)
    s.set_defaults(fn=cmd_embed_code)
    s = sp.add_parser("code-search"); s.add_argument("query", nargs="?", default="")
    s.add_argument("--like", metavar="ENTRY", help="functions similar to this one"); s.add_argument("-k", type=int, default=10)
    # One query embeds fast on the CPU, and the GPU is often full with Ollama.
    s.add_argument("--tier", type=int); s.add_argument("--gpu", action="store_true")
    s.add_argument("--min-size", type=int, default=120, help="bytes; tiny stubs embed close to every query")
    s.set_defaults(fn=cmd_code_search)
    s = sp.add_parser("import-jp-refs"); s.add_argument("file"); s.set_defaults(fn=cmd_import_jp_refs)
    s = sp.add_parser("status"); s.set_defaults(fn=cmd_status)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
