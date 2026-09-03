#!/usr/bin/env bash
set -euo pipefail
bin=$1
suffix=$$
market=/helix_signal_market_${suffix}
results=/helix_signal_results_${suffix}
socket=/tmp/helix_signal_${suffix}.sock
pids=()
cleanup(){ for pid in "${pids[@]:-}"; do kill -TERM "$pid" 2>/dev/null || true; done; rm -f "$socket"; }
trap cleanup EXIT
"$bin/helix_market_data" --market "$market" --results "$results" --socket "$socket" >/dev/null & pids+=($!)
"$bin/helix_pricer" --market "$market" --results "$results" --socket "$socket" --universe 512 >/dev/null & pids+=($!)
"$bin/helix_risk" --market "$market" --results "$results" --socket "$socket" >/dev/null & pids+=($!)
for _ in $(seq 1 100); do [[ -S "$socket" ]] && break; sleep .05; done
[[ -S "$socket" ]]
healthy=0
for _ in $(seq 1 120); do
  status=$("$bin/helixctl" status --market "$market" --results "$results" --socket "$socket" 2>/dev/null || true)
  if [[ "$status" == *'"pricer":{"state":"healthy"'* &&
        "$status" == *'"risk":{"state":"healthy"'* ]]; then healthy=1; break; fi
  sleep .05
done
[[ $healthy -eq 1 ]]
kill -TERM "${pids[0]}"
for _ in $(seq 1 100); do
  alive=0
  for pid in "${pids[@]}"; do kill -0 "$pid" 2>/dev/null && alive=1; done
  [[ $alive -eq 0 ]] && break
  sleep .05
done
for pid in "${pids[@]}"; do ! kill -0 "$pid" 2>/dev/null; wait "$pid"; done
echo "signal shutdown propagation passed"
