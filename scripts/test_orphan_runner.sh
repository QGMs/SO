#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
RUNTIME_DIR="${TP_RUNTIME_DIR:-/tmp/so_tp_$(id -u)}"

mkdir -p tmp
rm -f "$RUNTIME_DIR/controller.fifo" tmp/controller.log \
  tmp/t_orphan_controller.out tmp/t_orphan_controller.err \
  tmp/t_orphan_runner.out tmp/t_orphan_runner.err \
  tmp/t_orphan_shutdown.out tmp/t_orphan_shutdown.err

./bin/controller 1 fifo >tmp/t_orphan_controller.out 2>tmp/t_orphan_controller.err &
CTRL_PID=$!
RUNNER_PID=""

cleanup() {
  if [[ -n "${RUNNER_PID}" ]] && kill -0 "$RUNNER_PID" 2>/dev/null; then
    kill -9 "$RUNNER_PID" >/dev/null 2>&1 || true
    wait "$RUNNER_PID" 2>/dev/null || true
  fi
  if kill -0 "$CTRL_PID" 2>/dev/null; then
    ./bin/runner -s >/dev/null 2>&1 || true
    wait "$CTRL_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

sleep 0.3

./bin/runner -e 7 sleep 10 >tmp/t_orphan_runner.out 2>tmp/t_orphan_runner.err &
RUNNER_PID=$!

# Espera ate o runner entrar em "executing", para garantir que estava realmente em running.
STARTED=0
for _ in $(seq 1 50); do
  if grep -q "executing command" tmp/t_orphan_runner.out; then
    STARTED=1
    break
  fi
  sleep 0.1
done

if (( STARTED == 0 )); then
  echo "[test_orphan_runner] FAIL: runner did not reach executing state" >&2
  exit 1
fi

kill -9 "$RUNNER_PID" >/dev/null 2>&1 || true
wait "$RUNNER_PID" 2>/dev/null || true
RUNNER_PID=""

T0="$(date +%s%3N)"
./bin/runner -s >tmp/t_orphan_shutdown.out 2>tmp/t_orphan_shutdown.err
T1="$(date +%s%3N)"
ELAPSED_MS=$((T1 - T0))

wait "$CTRL_PID"
trap - EXIT

if (( ELAPSED_MS > 5000 )); then
  echo "[test_orphan_runner] FAIL: shutdown took too long (${ELAPSED_MS}ms)" >&2
  exit 1
fi

if ! grep -q "exit=255" tmp/controller.log; then
  echo "[test_orphan_runner] FAIL: expected orphan command with exit=255 in log" >&2
  exit 1
fi

echo "[test_orphan_runner] elapsed_ms=${ELAPSED_MS}"
echo "[test_orphan_runner] PASS"
