# pure

OFR(Optimized Full-page Recovery) InnoDB storage engine variants. Each top-level
folder contains only the modified `storage/innobase` tree — drop it into a
MySQL 8.0.24 source checkout (`storage/innobase`) and rebuild to swap engines.

## storage_fullpage

Baseline OFR engine. On the first X/SX-latch of a page by a transaction, the
whole page is captured as a before-image (`capture_before_image()`), mmap'd
into an NVDIMM-backed file. On crash recovery, pages belonging to uncommitted
transactions are restored wholesale from that image; pages belonging to
committed transactions are left untouched via a persisted commit sentinel.
Includes an early row-level delta path for pages touched by more than one
in-flight transaction, but variable-length (shrink) updates on those pages
are not yet handled safely.

## storage_eviction

Same full-page capture core, plus NVDIMM capacity management. The
before-image region on NVDIMM is fixed-size; once usage crosses a threshold,
a background thread asynchronously evicts valid (uncommitted) page images to
a second file on SSD (`second-rcp.bin`), while new NVDIMM appends continue
uninterrupted. Each page's meta entry tracks `storage_loc` (NVDIMM or SSD) so
recovery reads from wherever the image actually ended up.

## storage_PURE

Full engine: adds correctness for variable-length updates on top of
`storage_fullpage`. Shrink/grow updates and inserts are captured as
row-level deltas (`OfrDeltaEntry`) instead of relying on a stale full-page
image, with two recovery-time schedules:

- **ASYNC** — same-size in-place overwrites, whose byte offset can never
  move; applied directly via `fil_io`, no buffer pool, no latch, no
  ordering requirement.
- **SYNC** — shrink updates and inserts, whose offset could be invalidated
  by another operation on the page; applied through the buffer pool under
  an `RW_X_LATCH` so patches sharing a page are serialized.

If a page is physically reorganized (`btr_page_reorganize`) while under
delta tracking, every delta captured for that page — regardless of
transaction — is routed to a **vanilla fallback**: real InnoDB undo log
replay via each row's own `roll_ptr`, since byte offsets on a reorganized
page can no longer be trusted. Commit also invalidates the delta-mode
sentinel (not just the full-page one), so a transaction that commits before
an unrelated later crash is never mistaken for uncommitted and reverted.

## Setup

### NVDIMM (DAX-mounted persistent memory)

OFR hardcodes its three backing files under `/mnt/pmem/` (`opt-for-recovery.bin`,
`recovery_tmp.bin`, `delta-undo.bin`, and `second-rcp.bin` in `storage_eviction`).
This must be a DAX mount so writes land on real persistent memory instead of
going through the page cache.

```bash
# list NVDIMM regions/namespaces (requires ndctl)
sudo ndctl list -R
sudo ndctl list

# (re)create the namespace in fsdax mode if it's currently "raw" --
# this destroys any existing namespace on that region
sudo ndctl destroy-namespace namespace1.0 -f
sudo ndctl create-namespace -r region1 -m fsdax

# format and mount with dax
sudo mkfs.ext4 /dev/pmem1
sudo mkdir -p /mnt/pmem
sudo mount -o dax /dev/pmem1 /mnt/pmem
```

To persist across reboots, add to `/etc/fstab`:
```
/dev/pmem1 /mnt/pmem ext4 dax 0 0
```

If `HAVE_LIBPMEM` is not defined at build time, `ofr_persist()` is a no-op and
durability relies on normal CPU cache eviction rather than an explicit
`pmem_persist`/`clflush` call — writes still reach the DAX-mounted file, just
without an explicit flush barrier.

### io_uring

The full-page recovery scan (Phase 1) uses `io_uring` when available, falling
back to plain `pread`/`pwrite` otherwise. Requires Linux 5.1+ and
`liburing-dev`:

```bash
sudo apt-get install liburing-dev
```

`storage/innobase/CMakeLists.txt` auto-detects it (`FIND_LIBRARY(URING_LIBRARY
uring)`) — no extra CMake flags needed; check the configure output for
`[OFR] Found liburing: ... – io_uring recovery enabled.`

### Building (example: storage_fullpage)

```bash
# 1. get vanilla MySQL 8.0.24 source
wget https://downloads.mysql.com/archives/get/p/23/file/mysql-boost-8.0.24.tar.gz
tar xzf mysql-boost-8.0.24.tar.gz
cd mysql-8.0.24

# 2. swap in the OFR engine
rm -rf storage/innobase
cp -r /path/to/pure/storage_fullpage/storage/innobase storage/innobase

# 3. configure + build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DWITH_BOOST=../boost \
         -DDOWNLOAD_BOOST=OFF
cmake --build . --target mysqld -j "$(nproc)"

# 4. initialize a data directory and start
./bin/mysqld --defaults-file=/path/to/my.cnf --initialize-insecure
./bin/mysqld --defaults-file=/path/to/my.cnf &
```

The same steps apply to `storage_eviction` and `storage_PURE` — just point
step 2 at the matching folder. A minimal `my.cnf` needs at least:

```ini
[mysqld]
datadir=/path/to/datadir
innodb_flush_method=O_DIRECT
innodb_page_size=4KB
innodb_log_file_size=256M
```

### Microbenchmark setup (sysbench)

```bash
sudo apt-get install sysbench   # 1.0.20 used here

sysbench oltp_read_write \
  --mysql-host=127.0.0.1 --mysql-port=3306 \
  --mysql-user=root --mysql-password=1234 \
  --mysql-db=sbtest \
  --tables=8 --table-size=1000000 \
  prepare

sysbench oltp_read_write \
  --mysql-host=127.0.0.1 --mysql-port=3306 \
  --mysql-user=root --mysql-password=1234 \
  --mysql-db=sbtest \
  --tables=8 --table-size=1000000 \
  --threads=32 --time=0 --report-interval=1 \
  run
```

### Crash-recovery scenario script

`scripts/crash_microbench.sh` automates: start mysqld → run a sysbench
microbenchmark under load → `kill -9` mid-run → restart → confirm the server
comes back up and report how long recovery took from the error log. See the
script for configurable paths (`MYSQLD_BIN`, `DEFAULTS_FILE`, `SOCK`) and
timing (`WARMUP_SECS` before the kill).

```bash
./scripts/crash_microbench.sh
```
