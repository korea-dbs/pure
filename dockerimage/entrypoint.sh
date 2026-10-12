#!/usr/bin/env bash
# Initializes the data directory on first run, then execs whatever command
# the container was started with (defaults to bash -- run
# /scripts/crash_microbench.sh from inside the container, or pass it
# directly as the docker run command).
set -e

mkdir -p /mnt/pmem /var/lib/mysql-data

if [ ! -d /var/lib/mysql-data/mysql ]; then
  echo "[entrypoint] initializing data directory..."
  "$MYSQLD_BIN" --defaults-file="$DEFAULTS_FILE" --initialize-insecure
fi

exec "$@"
