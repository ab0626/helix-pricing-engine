#!/usr/bin/env bash
set -euo pipefail

bin_dir=$1
suffix=$$
market="/helix_supervisor_market_${suffix}"
results="/helix_supervisor_results_${suffix}"
socket="/tmp/helix_supervisor_${suffix}.sock"
log="/tmp/helix_supervisor_${suffix}.log"
supervisor_pid=

cleanup() {
  local rc=$?
  if [[ -n "${supervisor_pid}" ]]; then
    kill -TERM "${supervisor_pid}" 2>/dev/null || true
    for _ in $(seq 1 100); do
      ! kill -0 "${supervisor_pid}" 2>/dev/null && break
      sleep 0.05
    done
    if kill -0 "${supervisor_pid}" 2>/dev/null; then
      kill -KILL "${supervisor_pid}" 2>/dev/null || true
    fi
    wait "${supervisor_pid}" 2>/dev/null || true
  fi
  if [[ ${rc} -ne 0 && -f "${log}" ]]; then cat "${log}"; fi
  rm -f "${socket}" "${log}"
  return ${rc}
}
trap cleanup EXIT

"${bin_dir}/helix_supervisor" --market "${market}" --results "${results}" \
  --socket "${socket}" --universe 512 --workers 2 --ring-capacity 8 \
  --drop-policy block >"${log}" 2>&1 &
supervisor_pid=$!

for _ in $(seq 1 100); do
  [[ -S "${socket}" ]] && break
  sleep 0.05
done
[[ -S "${socket}" ]]

market_before=
pricer_before=
for _ in $(seq 1 100); do
  status_before=$("${bin_dir}/helixctl" status --market "${market}" \
    --results "${results}" --socket "${socket}" 2>/dev/null || true)
  market_before=$(sed -n 's/.*"market":{"state":"[^"]*","pid":\([0-9]*\).*/\1/p' <<<"${status_before}")
  pricer_before=$(sed -n 's/.*"pricer":{"state":"[^"]*","pid":\([0-9]*\).*/\1/p' <<<"${status_before}")
  [[ -n "${market_before}" && -n "${pricer_before}" && "${pricer_before}" != "0" ]] && break
  sleep 0.05
done
[[ -n "${market_before}" && -n "${pricer_before}" ]]
kill -KILL "${pricer_before}"

market_after=
pricer_after=
for _ in $(seq 1 160); do
  if [[ -S "${socket}" ]]; then
    status_after=$("${bin_dir}/helixctl" status --market "${market}" \
      --results "${results}" --socket "${socket}" 2>/dev/null || true)
    market_after=$(sed -n 's/.*"market":{"state":"[^"]*","pid":\([0-9]*\).*/\1/p' <<<"${status_after}")
    pricer_after=$(sed -n 's/.*"pricer":{"state":"[^"]*","pid":\([0-9]*\).*/\1/p' <<<"${status_after}")
    [[ -n "${market_after}" && "${market_after}" != "${market_before}" &&
       -n "${pricer_after}" && "${pricer_after}" != "${pricer_before}" ]] && break
  fi
  sleep 0.05
done
[[ -n "${market_after}" && "${market_after}" != "${market_before}" ]]
[[ -n "${pricer_after}" && "${pricer_after}" != "${pricer_before}" ]]
echo "supervisor fault recovery passed: market ${market_before} -> ${market_after}, pricer ${pricer_before} -> ${pricer_after}"
