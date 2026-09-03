#!/usr/bin/env bash
set -euo pipefail
build_dir=${1:-build/native-release}
soak_seconds=${2:-3600}
mkdir -p native-results
{
  echo "validation_date=$(date --iso-8601=seconds)"
  uname -a
  lscpu
  echo "affinity=$(taskset -pc $$ 2>&1)"
  if chrt -f 1 true 2>/dev/null; then echo "sched_fifo=available"; else echo "sched_fifo=unavailable"; fi
} | tee native-results/environment.txt
cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHELIX_ENABLE_AVX2=ON -DHELIX_ENABLE_AVX512=ON -DHELIX_ENABLE_LTO=ON
cmake --build "$build_dir" -j "$(nproc)"
ctest --test-dir "$build_dir" --output-on-failure
"$build_dir/helix_bench" all 200000 | tee native-results/benchmarks.jsonl
"$build_dir/helix_ipc_bench" | tee -a native-results/benchmarks.jsonl
scripts/soak-test.sh "$build_dir" "$soak_seconds" | tee native-results/soak.log
