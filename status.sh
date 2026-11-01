#!/bin/bash
# One screen of status for the DSR background jobs. Live view: watch -t -n 5 ./status.sh (from the repo root)
D=$(cd "$(dirname "$0")" && pwd)
running() { pgrep -f "$1" >/dev/null && echo running || echo idle; }
echo "DSR jobs  $(date +%T)   load $(cut -d' ' -f1-3 /proc/loadavg)"
echo
python3 -I - "$D" <<'EOF'
import os, sqlite3, sys
d = sys.argv[1]
def ro(p):
    return sqlite3.connect(f"file:{p}?mode=ro", uri=True, timeout=1)
def bar(n, total, w=30):
    f = int(w * n / total) if total else 0
    return "[" + "#" * f + "." * (w - f) + f"] {n}/{total} ({100 * n / total if total else 0:.1f}%)"
try:
    c = ro(f"{d}/jp/jp-strings.sqlite")
    done, total = c.execute("SELECT count(en), count(*) FROM strings").fetchone()
    print("Translation  ", bar(done, total))
except sqlite3.Error as e:
    print("Translation   ?", e)
try:
    total = ro(f"{d}/fndb/dsr-functions.sqlite").execute(
        "SELECT count(*) FROM functions WHERE decomp IS NOT NULL AND decomp NOT LIKE '// %'").fetchone()[0]
    p = f"{d}/fndb/dsr-functions-code.sqlite"
    done = ro(p).execute("SELECT count(*) FROM code_map").fetchone()[0] if os.path.exists(p) else 0
    print("Code index   ", bar(done, total))
except sqlite3.Error as e:
    print("Code index    ?", e)
EOF
echo
echo "translate.py  $(running '^python3 -I translate.py')   $(tail -1 "$D/jp/translate.out" 2>/dev/null | cut -c1-90)"
echo "embed-code    $(running '^[^ ]*python[^ ]* -I fndb.py embed-code')   $(grep -v "^\s*$" "$D/fndb/work/embed-code.log" 2>/dev/null | tail -1 | cut -c1-80)"
echo "Ghidra        $(running '^[^ ]*java .*ghidra.Ghidra')"
echo
nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu --format=csv,noheader \
	| awk -F', ' '{print "GPU " $1 " busy, " $2 " / " $3 ", " $4 "C"}'
ollama ps 2>/dev/null | tail -n +2 | awk '{print "Ollama: " $1 " loaded, " $5 " " $6}'
