#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RESULT_FILE="tmp/test_requirements_report.txt"
RUNTIME_DIR="${TP_RUNTIME_DIR:-/tmp/so_tp_$(id -u)}"

mkdir -p tmp
: > "$RESULT_FILE"

log_step() {
  local msg="$1"
  printf '%s\n' "[test_requirements] $msg" | tee -a "$RESULT_FILE"
}

cleanup_env() {
  pkill -f '[.]\/bin\/controller' >/dev/null 2>&1 || true
  rm -f "$RUNTIME_DIR/controller.fifo" tmp/out.txt tmp/err.txt tmp/controller.log \
    tmp/t_req_*.out tmp/t_req_*.err tmp/t_req_*.txt >/dev/null 2>&1 || true
}

start_controller() {
  local parallel="$1"
  local policy="$2"
  ./bin/controller "$parallel" "$policy" >tmp/t_req_controller.out 2>tmp/t_req_controller.err &
  CTRL_PID=$!
  sleep 0.3
}

stop_controller() {
  ./bin/runner -s >tmp/t_req_shutdown.out 2>tmp/t_req_shutdown.err
  wait "$CTRL_PID" || true
}

assert_contains() {
  local file="$1"
  local pattern="$2"
  if ! grep -qE "$pattern" "$file"; then
    log_step "FAIL: expected pattern '$pattern' in $file"
    exit 1
  fi
}

assert_file_nonempty() {
  local file="$1"
  if [[ ! -s "$file" ]]; then
    log_step "FAIL: expected non-empty file $file"
    exit 1
  fi
}

cleanup_env
make clean >/dev/null
make all >/dev/null

log_step "PASS build"

# 1) Execucao simples + log persistente
start_controller 1 fifo
./bin/runner -e 1 echo hello >tmp/t_req_echo.out 2>tmp/t_req_echo.err
assert_contains tmp/t_req_echo.out 'hello'
assert_contains tmp/t_req_echo.out '\[runner\] command .* finished'
assert_contains tmp/controller.log 'cmd="echo hello"'
assert_contains tmp/controller.log 'exit=0'
stop_controller
log_step "PASS execucao simples e logging"

# 2) Consulta com fila (Executing + Scheduled)
start_controller 1 fifo
./bin/runner -e 2 sleep 2 >tmp/t_req_sleep.out 2>tmp/t_req_sleep.err &
PID_SLEEP=$!
./bin/runner -e 3 echo queued >tmp/t_req_queue.out 2>tmp/t_req_queue.err &
PID_QUEUED=$!
sleep 0.2
./bin/runner -c >tmp/t_req_list.out 2>tmp/t_req_list.err
assert_contains tmp/t_req_list.out '^Executing$'
assert_contains tmp/t_req_list.out '^Scheduled$'
assert_contains tmp/t_req_list.out 'user-id 2 - command-id'
assert_contains tmp/t_req_list.out 'user-id 3 - command-id'
wait "$PID_SLEEP"
wait "$PID_QUEUED"
stop_controller
log_step "PASS consulta de execucao/espera"

# 3) Operadores | > < 2>
start_controller 1 fifo
./bin/runner -e 1 "grep root /etc/passwd | wc -l > tmp/out.txt" >tmp/t_req_pipe1.out 2>tmp/t_req_pipe1.err
./bin/runner -e 2 "cat < /etc/passwd | wc -l" >tmp/t_req_pipe2.out 2>tmp/t_req_pipe2.err
set +e
./bin/runner -e 3 "ls /nao-existe 2> tmp/err.txt" >tmp/t_req_pipe3.out 2>tmp/t_req_pipe3.err
RC_LS=$?
set -e
if (( RC_LS != 2 )); then
  log_step "FAIL: expected ls /nao-existe to return 2, got ${RC_LS}"
  exit 1
fi
assert_file_nonempty tmp/out.txt
assert_file_nonempty tmp/err.txt
assert_contains tmp/controller.log 'cmd="ls /nao-existe 2> tmp/err.txt"'
assert_contains tmp/controller.log 'exit=2'
stop_controller
log_step "PASS operadores de redirecionamento e pipes"

# 4) Shutdown espera por runners ativos
start_controller 1 fifo
./bin/runner -e 9 sleep 2 >tmp/t_req_long.out 2>tmp/t_req_long.err &
PID_LONG=$!
sleep 0.2
T0=$(date +%s%3N)
./bin/runner -s >tmp/t_req_shutdown_wait.out 2>tmp/t_req_shutdown_wait.err
T1=$(date +%s%3N)
wait "$PID_LONG"
ELAPSED_WAIT=$((T1 - T0))
if (( ELAPSED_WAIT < 1200 )); then
  log_step "FAIL: shutdown wait too short (${ELAPSED_WAIT}ms)"
  exit 1
fi
log_step "PASS shutdown gracioso (wait=${ELAPSED_WAIT}ms)"

# 5) Paralelismo + politica
start_controller 2 rr
T0=$(date +%s%3N)
./bin/runner -e 1 sleep 2 >/dev/null &
P1=$!
./bin/runner -e 2 sleep 2 >/dev/null &
P2=$!
./bin/runner -e 3 sleep 2 >/dev/null &
P3=$!
./bin/runner -e 4 sleep 2 >/dev/null &
P4=$!
wait "$P1" "$P2" "$P3" "$P4"
T1=$(date +%s%3N)
ELAPSED_PAR=$((T1 - T0))
./bin/runner -s >/dev/null
wait "$CTRL_PID"
if (( ELAPSED_PAR > 7000 )); then
  log_step "FAIL: parallel execution too slow (${ELAPSED_PAR}ms)"
  exit 1
fi
assert_contains tmp/controller.log 'policy=rr'
assert_contains tmp/controller.log 'parallel=2'
log_step "PASS paralelismo e politica rr (elapsed=${ELAPSED_PAR}ms)"

log_step "ALL CHECKS PASSED"
echo
cat "$RESULT_FILE"
