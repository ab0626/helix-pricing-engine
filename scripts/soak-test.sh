#!/usr/bin/env bash
set -euo pipefail

bin_dir=${1:-./build/release}
duration=${2:-60}
"${bin_dir}/helix_demo" --duration "${duration}" --universe 4096 \
  --workers 4 --underlyings 8 --ring-capacity 8 --drop-policy block \
  --kernel scalar-fast --sync hybrid
