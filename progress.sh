#!/bin/bash
# Live progress of the DSR jobs, refreshed every 5 s (Ctrl-C quits). Works from the host or inside the Ubuntu distrobox.
# Usage: progress.sh [seconds]
D=$(cd "$(dirname "$0")" && pwd)
if [ ! -e /run/.containerenv ]; then
	exec distrobox enter Ubuntu -- "$D/progress.sh" "$@"
fi
while true; do
	out=$("$D/status.sh" 2>&1)
	clear
	echo "$out"
	sleep "${1:-5}"
done
