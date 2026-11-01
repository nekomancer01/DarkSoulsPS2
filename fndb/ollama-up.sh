#!/bin/bash
# Make sure an Ollama server with N parallel slots (default 4) is running; restart it if it has a different N.
# KV cache memory is num_ctx x slots, so large-context passes need fewer slots to stay in VRAM.
N=${1:-4}
HERE=$(dirname "$0")
STATE=$HERE/ollama.parallel
up() { curl -sf http://127.0.0.1:11434/api/version >/dev/null; }
if up && [ "$(cat "$STATE" 2>/dev/null)" = "$N" ]; then
	exit 0
fi
if up; then
	pkill -f "^ollama serve" || true
	for _ in $(seq 20); do up || break; sleep 0.5; done
fi
export OLLAMA_NUM_PARALLEL=$N OLLAMA_MAX_LOADED_MODELS=1 OLLAMA_KEEP_ALIVE=5m
export OLLAMA_FLASH_ATTENTION=1 OLLAMA_KV_CACHE_TYPE=q8_0  # halves KV memory
setsid nohup ollama serve > "$HERE/ollama.log" 2>&1 < /dev/null &
for _ in $(seq 30); do
	if up; then
		echo "$N" > "$STATE"
		exit 0
	fi
	sleep 1
done
echo "ollama did not start; see $HERE/ollama.log" >&2
exit 1
