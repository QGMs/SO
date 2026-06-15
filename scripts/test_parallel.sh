#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
RUNTIME_DIR="${TP_RUNTIME_DIR:-/tmp/so_tp_$(id -u)}"

mkdir -p tmp
rm -f "$RUNTIME_DIR/controller.fifo" tmp/controller.log tmp/controller.out tmp/controller.err

./bin/controller 2 rr >tmp/controller.out 2>tmp/controller.err &
CTRL_PID=$!

cleanup() {
  if kill -0 "$CTRL_PID" 2>/dev/null; then
    ./bin/runner -s >/dev/null 2>&1 || true
    wait "$CTRL_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

sleep 0.3

START_MS="$(date +%s%3N)"
RUNNER_PIDS=()

for i in 1 2 3 4; do
  ./bin/runner -e "$i" sleep 2 >/dev/null &
  RUNNER_PIDS+=("$!")
done

for pid in "${RUNNER_PIDS[@]}"; do
  wait "$pid"
done

END_MS="$(date +%s%3N)"
ELAPSED_MS=$((END_MS - START_MS))

./bin/runner -s >/dev/null
wait "$CTRL_PID"
trap - EXIT

echo "[test_parallel] elapsed_ms=$ELAPSED_MS"
if (( ELAPSED_MS > 6500 )); then
  echo "[test_parallel] FAIL: elapsed time too high for parallel=2" >&2
  exit 1
fi

echo "[test_parallel] PASS"
