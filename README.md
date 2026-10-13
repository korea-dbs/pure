# PURE

PURE InnoDB storage engine variants. Each top-level
folder contains only the modified `storage/innobase` tree — drop it into a
MySQL 8.0.24 source checkout (`storage/innobase`) and rebuild to swap engines.

## storage_fullpage

Baseline PURE engine. On the first X/SX-latch of a page by a transaction, the
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
`storage_fullpage`.

## Setup

### NVDIMM (DAX-mounted persistent memory)

PURE hardcodes its three backing files under `/mnt/pmem/` .
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

# 2. swap in the PURE engine
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
step 2 at the matching folder (steps 1-4 of `dockerimage/Dockerfile` do
exactly this for `storage_fullpage` if you'd rather not do it by hand).
A minimal `my.cnf` needs at least:

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

`scripts/crash_microbench.sh` automates a single-client example: start
mysqld → prepare a `sbtest1` table via sysbench → one client repeatedly
runs a fixed-length update (`UPDATE sbtest1 SET k = k + 1 WHERE id <
10000`) → crash the server with `kill -SIGABRT $(pidof mysqld)` mid-update
→ restart → report how long recovery took (from the signal to "ready for
connections") and print the relevant PURE recovery log lines. 

```bash
MYSQLD_BIN=/path/to/build/bin/mysqld \
DEFAULTS_FILE=/path/to/my.cnf \
ERROR_LOG=/path/to/datadir/mysql_error.log \
./scripts/crash_microbench.sh
```

### Docker image

`dockerimage/` builds `storage_fullpage` end-to-end from vanilla MySQL
8.0.24 inside a container, with sysbench and `scripts/crash_microbench.sh`
already in place:

```bash
# from the repo root (needs storage_fullpage/ and scripts/ as build context)
docker build -f dockerimage/Dockerfile -t ofr-fullpage .
docker run -it --rm ofr-fullpage
# inside the container:
/scripts/crash_microbench.sh
```

There's no real NVDIMM/DAX inside a container, so `/mnt/pmem` is just a
regular directory on the container's filesystem — the crash-recovery logic
is unaffected, you just don't get real persistent-memory latency/durability.
To use a real DAX device, bind-mount it: `docker run -v /mnt/pmem:/mnt/pmem
-it --rm ofr-fullpage` on a host that has one set up (see the NVDIMM
section above).
