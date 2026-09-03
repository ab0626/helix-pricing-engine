#!/bin/sh
set -eu
command -v perf >/dev/null 2>&1 || { echo "perf is unavailable" >&2; exit 2; }
exec perf stat -e cycles,instructions,cache-references,cache-misses,context-switches,cpu-migrations "$@"
