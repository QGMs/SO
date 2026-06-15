#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
RUNTIME_DIR="${TP_RUNTIME_DIR:-/tmp/so_tp_$(id -u)}"

JOBS="${1:-12}"
USERS="${2:-3}"
PARALLEL="${3:-2}"
RUNS="${4:-3}"

SUMMARY_FILE="tmp/benchmark_results.tsv"
DETAIL_FILE="tmp/benchmark_runs.tsv"

mkdir -p tmp
rm -f "$SUMMARY_FILE" "$DETAIL_FILE" \
  tmp/benchmark_*.log tmp/benchmark_*.out tmp/benchmark_*.err \
  tmp/controller.log tmp/controller.out tmp/controller.err

echo -e "policy\trun\tuser\tcount\tavg_wait_ms\tavg_total_ms" > "$DETAIL_FILE"
echo -e "policy\tuser\tcount\tavg_wait_ms\tavg_total_ms" > "$SUMMARY_FILE"

aggregate_log() {
  local log_file="$1"
  local policy="$2"
  local run_label="$3"

  awk -v policy="$policy" -v run_label="$run_label" '
  {
    user=""; wait_ms=""; total_ms="";
    for (i = 1; i <= NF; i++) {
      split($i, a, "=");
      if (a[1] == "user") user = a[2];
      if (a[1] == "wait_ms") wait_ms = a[2];
      if (a[1] == "total_ms") total_ms = a[2];
    }
    if (user != "" && wait_ms != "" && total_ms != "") {
      cnt[user] += 1;
      wait_sum[user] += wait_ms;
      total_sum[user] += total_ms;
    }
  }
  END {
    for (u in cnt) {
      printf "%s\t%s\t%s\t%d\t%.2f\t%.2f\n",
             policy, run_label, u, cnt[u], wait_sum[u] / cnt[u], total_sum[u] / cnt[u];
    }
  }' "$log_file" | sort -k3,3n
}

for POLICY in fifo rr; do
  COMBINED_LOG="tmp/benchmark_${POLICY}_all.log"
  : > "$COMBINED_LOG"

  for RUN in $(seq 1 "$RUNS"); do
    LOG_FILE="tmp/benchmark_${POLICY}_run${RUN}.log"
    OUT_FILE="tmp/benchmark_${POLICY}_run${RUN}.out"
    ERR_FILE="tmp/benchmark_${POLICY}_run${RUN}.err"

    rm -f "$RUNTIME_DIR/controller.fifo" tmp/controller.log "$OUT_FILE" "$ERR_FILE"

    ./bin/controller "$PARALLEL" "$POLICY" >"$OUT_FILE" 2>"$ERR_FILE" &
    CTRL_PID=$!
    sleep 0.3

    RUNNER_PIDS=()
    for ((i=1; i<=JOBS; i++)); do
      USER_ID=$(( ((i - 1) % USERS) + 1 ))
      SLEEP_SECS=$(( (((i - 1) / USERS) % 3) + 1 ))
      ./bin/runner -e "$USER_ID" sleep "$SLEEP_SECS" >/dev/null &
      RUNNER_PIDS+=("$!")
    done

    for pid in "${RUNNER_PIDS[@]}"; do
      wait "$pid"
    done

    ./bin/runner -s >/dev/null
    wait "$CTRL_PID"

    cp tmp/controller.log "$LOG_FILE"
    cat "$LOG_FILE" >> "$COMBINED_LOG"
    aggregate_log "$LOG_FILE" "$POLICY" "$RUN" >> "$DETAIL_FILE"
  done

  aggregate_log "$COMBINED_LOG" "$POLICY" "all" | awk -F '\t' 'BEGIN { OFS="\t" } { print $1, $3, $4, $5, $6 }' >> "$SUMMARY_FILE"
done

echo "[benchmark] wrote $SUMMARY_FILE"
column -t -s $'\t' "$SUMMARY_FILE"
