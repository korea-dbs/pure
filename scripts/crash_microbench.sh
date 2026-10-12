#!/usr/bin/env bash
# crash_microbench.sh
#
# Runs a sysbench oltp_read_write microbenchmark against a built mysqld,
# kills the server with SIGKILL mid-run, restarts it, and reports how long
# crash recovery took (from the kill to "ready for connections" in the
# error log). Works against any of the storage_fullpage / storage_eviction /
# storage_PURE builds -- just point MYSQLD_BIN/DEFAULTS_FILE at that build.
#
# Usage: ./crash_microbench.sh
set -euo pipefail

# ---- configure these for your setup ----------------------------------
MYSQLD_BIN="${MYSQLD_BIN:-/path/to/build/bin/mysqld}"
DEFAULTS_FILE="${DEFAULTS_FILE:-/path/to/my.cnf}"
ERROR_LOG="${ERROR_LOG:-/path/to/datadir/mysql_error.log}"
SOCK="${SOCK:-/tmp/mysql.sock}"
DB_USER="${DB_USER:-root}"
DB_PASS="${DB_PASS:-1234}"
DB_NAME="${DB_NAME:-sbtest}"
SB_TABLES="${SB_TABLES:-8}"
SB_TABLE_SIZE="${SB_TABLE_SIZE:-1000000}"
SB_THREADS="${SB_THREADS:-32}"
WARMUP_SECS="${WARMUP_SECS:-15}"   # how long sysbench runs before the kill
READY_MARKER="ready for connections"
# ------------------------------------------------------------------------

mysql_cli() { mysql -S "$SOCK" -u"$DB_USER" -p"$DB_PASS" "$@" 2>&1 | grep -v Warning; }

wait_for_marker() {
  local start_byte
  start_byte=$(stat -c%s "$ERROR_LOG" 2>/dev/null || echo 0)
  local t0=$(date +%s.%N)
  while true; do
    if tail -c +"$((start_byte + 1))" "$ERROR_LOG" 2>/dev/null | grep -q "$READY_MARKER"; then
      local t1=$(date +%s.%N)
      echo "$t1 $t0" # caller computes elapsed
      return 0
    fi
    sleep 0.2
  done
}

start_mysqld() {
  nohup "$MYSQLD_BIN" --defaults-file="$DEFAULTS_FILE" >> "$ERROR_LOG" 2>&1 &
  echo $!
}

echo "[crash-bench] starting mysqld..."
MYSQLD_PID=$(start_mysqld)
read -r T1 T0 < <(wait_for_marker)
echo "[crash-bench] server up (pid=$MYSQLD_PID)"

echo "[crash-bench] preparing sysbench schema (skips if already present)..."
mysql_cli -e "CREATE DATABASE IF NOT EXISTS $DB_NAME;" || true
sysbench oltp_read_write \
  --mysql-socket="$SOCK" --mysql-user="$DB_USER" --mysql-password="$DB_PASS" \
  --mysql-db="$DB_NAME" --tables="$SB_TABLES" --table-size="$SB_TABLE_SIZE" \
  prepare || true

echo "[crash-bench] starting sysbench load in background for ${WARMUP_SECS}s before the crash..."
sysbench oltp_read_write \
  --mysql-socket="$SOCK" --mysql-user="$DB_USER" --mysql-password="$DB_PASS" \
  --mysql-db="$DB_NAME" --tables="$SB_TABLES" --table-size="$SB_TABLE_SIZE" \
  --threads="$SB_THREADS" --time=0 --report-interval=1 \
  run > /tmp/crash_bench_sysbench.log 2>&1 &
SYSBENCH_PID=$!

sleep "$WARMUP_SECS"

echo "[crash-bench] killing mysqld (pid=$MYSQLD_PID) with SIGKILL..."
CRASH_T0=$(date +%s.%N)
kill -9 "$MYSQLD_PID"
kill "$SYSBENCH_PID" 2>/dev/null || true
wait "$MYSQLD_PID" 2>/dev/null || true
sleep 1

echo "[crash-bench] restarting mysqld..."
MYSQLD_PID=$(start_mysqld)
read -r T1 T0 < <(wait_for_marker)
RECOVERY_SECS=$(echo "$T1 - $CRASH_T0" | bc)

echo "[crash-bench] server back up (pid=$MYSQLD_PID)"
echo "[crash-bench] time from kill -9 to ready-for-connections: ${RECOVERY_SECS}s"
echo "[crash-bench] relevant recovery log lines:"
grep -E "Recovery phase|OFR-recover|OFR\]|delta undo|pending-vanilla" "$ERROR_LOG" | tail -30
