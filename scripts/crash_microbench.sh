#!/usr/bin/env bash
# crash_microbench.sh
#
# Example crash-recovery test: a single client repeatedly runs a
# fixed-length UPDATE (sbtest1.k = k + 1) over rows id < ID_CEILING while
# mysqld is running, then the server is crashed with SIGABRT mid-update and
# restarted. Reports how long recovery took (from the crash to "ready for
# connections" in the error log) and shows the relevant OFR log lines.
#
# Because every update is the same fixed-size INT column, this exercises
# OFR's ASYNC delta path (same-size in-place overwrite, no page reorganize).
#
# Works against any of the storage_fullpage / storage_eviction /
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
SB_TABLE_SIZE="${SB_TABLE_SIZE:-100000}"   # rows in sbtest1
ID_CEILING="${ID_CEILING:-10000}"          # single client updates id < this
WARMUP_SECS="${WARMUP_SECS:-15}"           # how long the update loop runs before the crash
READY_MARKER=": ready for connections"  # the main "mysqld: ready for..." line,
                                          # not the earlier "X Plugin ready for
                                          # connections" one (no socket to
                                          # connect to yet at that point)
# ------------------------------------------------------------------------

mysql_cli() { mysql -S "$SOCK" -u"$DB_USER" -p"$DB_PASS" "$@" 2>&1 | grep -v Warning; }

wait_for_marker() {
  local start_byte
  start_byte=$(stat -c%s "$ERROR_LOG" 2>/dev/null || echo 0)
  while true; do
    if tail -c +"$((start_byte + 1))" "$ERROR_LOG" 2>/dev/null | grep -q "$READY_MARKER"; then
      date +%s.%N
      return 0
    fi
    sleep 0.2
  done
}

start_mysqld() {
  nohup "$MYSQLD_BIN" --defaults-file="$DEFAULTS_FILE" >> "$ERROR_LOG" 2>&1 &
}

echo "[crash-bench] starting mysqld..."
start_mysqld
wait_for_marker > /dev/null
echo "[crash-bench] server up (pid=$(pidof mysqld))"

echo "[crash-bench] preparing sbtest1 (single table, ${SB_TABLE_SIZE} rows; skips if already present)..."
mysql_cli -e "CREATE DATABASE IF NOT EXISTS $DB_NAME;" || true
sysbench oltp_read_write \
  --mysql-socket="$SOCK" --mysql-user="$DB_USER" --mysql-password="$DB_PASS" \
  --mysql-db="$DB_NAME" --tables=1 --table-size="$SB_TABLE_SIZE" \
  prepare || true

echo "[crash-bench] single-client fixed-length update loop (sbtest1.k = k+1, id < ${ID_CEILING}) for ${WARMUP_SECS}s..."
(
  while true; do
    mysql_cli -D "$DB_NAME" -e "UPDATE sbtest1 SET k = k + 1 WHERE id < $ID_CEILING;" >/dev/null 2>&1 || true
  done
) &
UPDATE_LOOP_PID=$!

sleep "$WARMUP_SECS"

echo "[crash-bench] crashing mysqld with: kill -SIGABRT \$(pidof mysqld)"
CRASH_T0=$(date +%s.%N)
kill -SIGABRT "$(pidof mysqld)"
kill "$UPDATE_LOOP_PID" 2>/dev/null || true
wait "$UPDATE_LOOP_PID" 2>/dev/null || true
sleep 1

echo "[crash-bench] restarting mysqld..."
start_mysqld
T1=$(wait_for_marker)
RECOVERY_SECS=$(echo "$T1 - $CRASH_T0" | bc)

echo "[crash-bench] server back up (pid=$(pidof mysqld))"
echo "[crash-bench] time from SIGABRT to ready-for-connections: ${RECOVERY_SECS}s"
echo "[crash-bench] relevant recovery log lines:"
grep -E "Recovery phase|OFR-recover|OFR\]|delta undo|pending-vanilla" "$ERROR_LOG" | tail -30
