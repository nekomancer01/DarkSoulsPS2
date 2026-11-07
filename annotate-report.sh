#!/bin/bash
# Usage: ./annotate-report.sh [REPORT] [--port N] [--no-open]
#
# Look through a play report (./play-report.sh) in your browser and pin comments on its screenshots. Opens the newest
# zip in build/play-reports/ (or the zip or unpacked folder you name) on a local page: step through the shots, click
# the image to pin a numbered comment, write a note for the shot or the whole report. Everything is saved into the
# report as you go (annotations/ in the zip: notes.md, notes.json, and each annotated shot with its pins drawn on it),
# so the same zip is the one to send. Ctrl+C here when done.
#
#   --port N     serve on 127.0.0.1:N (default: any free port)
#   --no-open    print the address only, do not open a browser
#
# Needs python3 only.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
case "${1:-}" in -h|--help) awk 'NR > 1 && !/^#/ {exit} NR > 1' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;; esac
command -v python3 >/dev/null || { echo "annotate-report: needs python3" >&2; exit 1; }
exec python3 -I "$ROOT/tools/annotate_report.py" "$@"
