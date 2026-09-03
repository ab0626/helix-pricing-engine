#!/bin/sh
set -eu
output=${1:-helix-benchmarks.jsonl}
binary_dir=${HELIX_BIN_DIR:-.}
: > "$output"
"$binary_dir/helix_bench" all 200000 >> "$output"
"$binary_dir/helix_ipc_bench" >> "$output"
printf '{"metadata":{"date":"%s","uname":"%s"}}\n' "$(date -u +%FT%TZ)" "$(uname -a)" >> "$output"
echo "wrote $output"
