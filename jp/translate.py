"""Translate the Japanese strings in jp-strings.sqlite to English with a local Ollama model.
Resumable: each batch is committed as it finishes, and a rerun continues with untranslated rows.
Glossary terms (glossary.py) found in a batch are added to its prompt. --redo re-translates rows that contain a
glossary term or whose English still has Japanese/Chinese characters; the previous English is kept in prev_en.
Usage: translate.py [--model M] [--batch 20] [--ctx 2048] [--workers 2] [--limit N] [--dry] [--redo]"""
import argparse, json, os, signal, sqlite3, time, urllib.request
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait

HERE = os.path.dirname(os.path.abspath(__file__))
# Code and data definitions first; param row names and cutscene data last.
ORDER = """CASE WHEN sources LIKE 'DarkSouls%' THEN 0 WHEN sources LIKE 'paramdef%' THEN 1 WHEN sources LIKE 'script%' THEN 2
    WHEN sources LIKE 'event%' THEN 3 WHEN sources LIKE 'map%' THEN 4 WHEN sources LIKE 'param%' THEN 5 ELSE 6 END"""
SYSTEM = """You translate Japanese strings from Dark Souls game files (debug messages, data field names, event names,
param row names, map part names) into short, literal English. Keep printf specifiers (%s %d), numbers, IDs, codes like
c5290 or m10_00, brackets and ASCII text unchanged. Use Dark Souls names where obvious (篝火 = bonfire, 白竜シース =
Seath the Scaleless). Input is a JSON object of numbered strings; return an object with the same numbers, each mapped
to the English for that string. Use the glossary's English for any glossary term that appears."""
CJK = "[\u3040-\u30ff\u4e00-\u9fff]"
stop = False
GLOSSARY = {}  # first two characters -> [(jp, en)], longest first


def load_glossary(con):
    for jp, en in con.execute("SELECT jp, en FROM glossary ORDER BY length(jp) DESC"):
        GLOSSARY.setdefault(jp[:2], []).append((jp, en))


def terms(strings, cap=40):
    """Glossary entries occurring in the strings, longest match at each position."""
    found = {}
    for s in strings:
        for i in range(len(s) - 1):
            for jp, en in GLOSSARY.get(s[i:i + 2], ()):
                if s.startswith(jp, i):
                    found[jp] = en
                    break
    return dict(list(found.items())[:cap])


def glossary_note(strings):
    g = terms(strings)
    return "\nGlossary:\n" + "\n".join(f"{j} = {e}" for j, e in g.items()) if g else ""


def ollama(a, strings):
    # Numbered keys, all required by the schema: a plain list let models drop or shift entries.
    keys = [str(i + 1) for i in range(len(strings))]
    body = {"model": a.model, "stream": False, "format": {"type": "object", "required": keys,
                                                          "properties": {k: {"type": "string"} for k in keys}},
        "options": {"temperature": 0.1, "num_ctx": a.ctx, "num_predict": 60 * len(strings)}, "keep_alive": "5m",
        "messages": [{"role": "system", "content": SYSTEM + glossary_note(strings)},
                     {"role": "user", "content": json.dumps(dict(zip(keys, strings)), ensure_ascii=False)}]}
    if a.model.startswith(("qwen3", "deepseek-r1")):
        body["think"] = False
    req = urllib.request.Request(a.host + "/api/chat", json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        out = json.loads(json.loads(r.read())["message"]["content"])
    return [out[k].strip() for k in keys]


def main():
    global stop
    p = argparse.ArgumentParser()
    p.add_argument("--model", default="qwen3:8b")
    p.add_argument("--host", default="http://127.0.0.1:11434")
    p.add_argument("--batch", type=int, default=20)
    p.add_argument("--ctx", type=int, default=2048, help="a small ctx keeps the KV cache, and so the model, on the GPU")
    p.add_argument("--workers", type=int, default=2)
    p.add_argument("--limit", type=int, default=10 ** 9)
    p.add_argument("--dry", action="store_true", help="print translations, do not save")
    p.add_argument("--redo", action="store_true", help="re-translate rows with glossary terms or leftover CJK")
    a = p.parse_args()
    con = sqlite3.connect(os.path.join(HERE, "jp-strings.sqlite"))
    if "prev_en" not in [r[1] for r in con.execute("PRAGMA table_info(strings)")]:
        con.execute("ALTER TABLE strings ADD COLUMN prev_en TEXT")
    if con.execute("SELECT name FROM sqlite_master WHERE name='glossary'").fetchone():
        load_glossary(con)
    if a.redo:
        import re
        cand = [s for s, en, model in con.execute(f"SELECT s, en, model FROM strings WHERE en IS NOT NULL ORDER BY {ORDER}")
                if not model.endswith("+glossary") and (terms([s]) or re.search(CJK, en))]
        rows = cand[:a.limit]
    else:
        rows = [r[0] for r in con.execute(f"SELECT s FROM strings WHERE en IS NULL ORDER BY {ORDER}, n DESC LIMIT ?",
                                          (a.limit,))]
    batches = [rows[i:i + a.batch] for i in range(0, len(rows), a.batch)]
    signal.signal(signal.SIGINT, lambda *_: globals().update(stop=True))
    print(f"{len(rows)} strings in {len(batches)} batches, {a.model}, {a.workers} workers; Ctrl-C stops after "
          "the batches in flight")
    done, failed, start = 0, 0, time.time()
    with ThreadPoolExecutor(a.workers) as pool:
        todo, running = iter(batches), {}
        while True:
            while not stop and len(running) < a.workers:
                b = next(todo, None)
                if b is None:
                    break
                running[pool.submit(ollama, a, b)] = b
            if not running:
                break
            finished, _ = wait(running, return_when=FIRST_COMPLETED)
            for f in finished:
                b = running.pop(f)
                try:
                    out = f.result()
                except Exception as e:
                    failed += len(b)
                    print("batch failed:", e)
                    continue
                if a.dry:
                    for s, t in zip(b, out):
                        print(f"{s}\t{t}")
                else:
                    now = time.strftime("%Y-%m-%d %H:%M:%S")
                    tag = a.model + ("+glossary" if GLOSSARY else "")
                    con.executemany("UPDATE strings SET prev_en=coalesce(prev_en, en), en=?, model=?, translated_at=? "
                                    "WHERE s=?", [(t, tag, now, s) for s, t in zip(b, out)])
                    con.commit()
                done += len(b)
                per = (time.time() - start) / done
                left = int(per * (len(rows) - done - failed))
                print(f"[{done}/{len(rows)} {100 * done / len(rows):.1f}%] {per:.2f}s/str, ETA "
                      f"{left // 3600}:{left // 60 % 60:02d}:{left % 60:02d} | {b[0][:30]} -> {out[0][:40]}", flush=True)
    print(f"translated {done}, failed {failed}" + ("; run again to continue" if done + failed < len(rows) else ""))


main()
