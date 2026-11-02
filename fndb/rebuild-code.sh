#!/bin/bash
# Waits for jp/translate.py to finish, frees the GPU, then rebuilds the code index so it includes the English strings.
cd "$(dirname "$0")"
# Python with fastembed + sqlite-vec for the vector steps: FNDB_PY, else .venv-fndb in the repo root, else python3
FNDB_PY=${FNDB_PY:-$([ -x ../.venv-fndb/bin/python ] && echo "$PWD/../.venv-fndb/bin/python" || echo python3)}
while pgrep -f "^python3 -I translate.py" >/dev/null; do sleep 20; done
curl -s localhost:11434/api/generate -d '{"model":"qwen3:8b","keep_alive":0}' >/dev/null
sleep 3
rm -f dsr-functions-code.sqlite dsr-functions-code.sqlite-journal
"$FNDB_PY" -I fndb.py embed-code
