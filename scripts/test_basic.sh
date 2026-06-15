#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
RUNTIME_DIR="${TP_RUNTIME_DIR:-/tmp/so_tp_$(id -u)}"

mkdir -p tmp
rm -f "$RUNTIME_DIR/controller.fifo" tmp/controller.log tmp/controller.out tmp/controller.err tmp/out_count.txt

./bin/controller 1 fifo >tmp/controller.out 2>tmp/controller.err &
CTRL_PID=$!

cleanup() {
  if kill -0 "$CTRL_PID" 2>/dev/null; then
    ./bin/runner -s >/dev/null 2>&1 || true
    wait "$CTRL_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

sleep 0.3

echo "[test_basic] Running simple command"
./bin/runner -e 1 echo hello

echo "[test_basic] Running pipeline with redirection"
./bin/runner -e 1 "grep root /etc/passwd | wc -l > tmp/out_count.txt"

echo "[test_basic] Running concurrent submissions"
./bin/runner -e 2 sleep 1 &
P1=$!
./bin/runner -e 3 echo queued &
P2=$!

sleep 0.2
./bin/runner -c

wait "$P1"
wait "$P2"

./bin/runner -s
wait "$CTRL_PID"
trap - EXIT

echo "[test_basic] controller.log"
cat tmp/controller.log

echo "[test_basic] out_count.txt"
cat tmp/out_count.txt

echo "[test_basic] PASS"
