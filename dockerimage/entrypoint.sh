#!/usr/bin/env bash
# Initializes the data directory on first run, then execs whatever command
# the container was started with (defaults to bash -- run
# /scripts/crash_microbench.sh from inside the container, or pass it
# directly as the docker run command).
set -e

mkdir -p /mnt/pmem /var/lib/mysql-data

ROOT_PASSWORD="${DB_PASS:-1234}"

if [ ! -d /var/lib/mysql-data/mysql ]; then
  echo "[entrypoint] initializing data directory..."
  "$MYSQLD_BIN" --defaults-file="$DEFAULTS_FILE" --initialize-insecure

  echo "[entrypoint] setting root password..."
  "$MYSQLD_BIN" --defaults-file="$DEFAULTS_FILE" &
  TMP_PID=$!
  for _ in $(seq 1 60); do
    mysqladmin --socket="$SOCK" ping >/dev/null 2>&1 && break
    sleep 0.5
  done
  mysql --socket="$SOCK" -uroot \
    -e "ALTER USER 'root'@'localhost' IDENTIFIED BY '${ROOT_PASSWORD}';"
  mysqladmin --socket="$SOCK" -uroot -p"${ROOT_PASSWORD}" shutdown
  wait "$TMP_PID" 2>/dev/null || true
fi

exec "$@"
