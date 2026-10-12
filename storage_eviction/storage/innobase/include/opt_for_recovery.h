 /* opt_for_recovery.h
 * Optimized Crash Recovery via Full-Page Before-Images.
 * Design overview
 * Before a transaction modifies a page for the first time (since the page was
 * last in a committed state) the engine captures the FULL, committed page
 * image and appends it to  opt-for-recovery.bin  (OFR file).
 * In-memory registry (OfrRegistry)
 *   key  : (trx_id, space_id, page_no)
 *   value: byte offset inside opt-for-recovery.bin where the image lives
 *
 * Second Storage (NVDIMM → SSD eviction)
 * When NVDIMM reaches OFR_EVICT_THRESHOLD % capacity a background thread
 * async-copies valid (uncommitted) pages to second-rcp.bin on SSD.
 * New NVDIMM appends continue uninterrupted during eviction.
 * After all SSD writes complete and meta entries are updated, the NVDIMM
 * write pointer is compacted/reset (capacity reclaimed).
 * The unified pageinfo in recovery_tmp.bin tracks storage_loc (NVDIMM vs SSD)
 * so recovery can read from the correct device.
 *****************************************************************************/

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "trx0types.h"   /* trx_id_t          */
#include "buf0types.h"   /* space_id_t, page_no_t, page_id_t */
#include "univ.i"        /* ulint, byte, etc. */

#ifndef OFR_RECOVERY_THREADS
#  define OFR_RECOVERY_THREADS 8
#endif

#ifndef OFR_URING_QUEUE_DEPTH
#  define OFR_URING_QUEUE_DEPTH 64
#endif

/* ── NVDIMM persist helper ───────────────────────────────────────────
 * ofr_persist(addr, len): flush CPU cache lines to NVDIMM media.
 *   - With libpmem: pmem_persist() = clwb per cache line + sfence
 *   - Without libpmem: msync() as fallback (syscall but correct)
 * On emulated NVDIMM (DRAM) this flushes to DRAM only; on real NVDIMM
 * it guarantees power-failure durability.                             */
#ifdef HAVE_LIBPMEM
#  include <libpmem.h>
#  define ofr_persist(addr, len)  pmem_persist((addr), (len))
#else
/* Lazy mode: skip explicit cache-line flush.
 * Writes reach NVDIMM via normal CPU cache eviction (like pwrite to page
 * cache).  Fast for normal execution; enable pmem_persist for strict
 * power-fail durability. */
#  define ofr_persist(addr, len)  ((void)0)
#endif

/* ── NVDIMM mmap sizes ───────────────────────────────────────────────
 * Files are stored on DAX-mounted NVDIMM (/mnt/pmem).
 * Pre-allocated at open() via fallocate; mapped with mmap(MAP_SHARED).
 * recovery_tmp.bin layout: [watermark:uint64_t][OfrMetaEntry...]
 *   watermark = absolute byte offset of the next write (>= sizeof(uint64_t))
 * opt-for-recovery.bin layout: [OfrPageHdr + page_data]...             */
static constexpr size_t   OFR_META_MAP_SIZE = 512ULL * 1024 * 1024;        /* 512 MB */
static constexpr size_t   OFR_FILE_MAP_SIZE = 14ULL * 1024 * 1024 * 1024; /* 14 GB  */
static constexpr uint64_t OFR_META_HDR_SIZE = sizeof(uint64_t); /* watermark field */

/* ── second storage constants ────────────────────────────────────── */
static constexpr uint8_t  STORAGE_NVDIMM      = 0; /*!< page image on NVDIMM        */
static constexpr uint8_t  STORAGE_SSD         = 1; /*!< page image evicted to SSD   */
/* Trigger async eviction when NVDIMM usage reaches this % of capacity.
 * Append-only writes continue during eviction → no TPS impact.
 * Tunable at compile time; default matches the paper's 50 % design.   */
static constexpr uint32_t OFR_EVICT_THRESHOLD = 50;

/* ── on-disk structures (packed, little-endian) ──────────────────── */
#pragma pack(push, 1)

struct OfrPageHdr {
  uint64_t trx_id;     /*!< owning transaction id          */
  uint32_t space_id;   /*!< tablespace id                  */
  uint32_t page_no;    /*!< page number inside tablespace  */
  uint32_t page_size;  /*!< page size in bytes             */
};

struct OfrMetaHdr {
  uint32_t magic;
  uint32_t n_entries;
};

struct OfrMetaEntry {
  uint64_t trx_id;     /*!< owning transaction id           */
  uint32_t space_id;   /*!< tablespace id                   */
  uint32_t page_no;    /*!< page number                     */
  uint64_t ofr_offset; /*!< byte offset in storage file     */
  uint32_t page_size;  /*!< page size in bytes              */
  uint8_t  storage_loc;/*!< STORAGE_NVDIMM(0) or STORAGE_SSD(1) */
  uint8_t  _pad[3];    /*!< explicit padding to 32 bytes    */
};

#pragma pack(pop)

static constexpr uint32_t OFR_META_MAGIC = 0x4F465200u; /* "OFR\0" */

struct OfrKey {
  trx_id_t  trx_id;
  space_id_t space_id;
  page_no_t  page_no;

  bool operator==(const OfrKey &o) const noexcept {
    return trx_id == o.trx_id &&
           space_id == o.space_id &&
           page_no == o.page_no;
  }
};

struct OfrKeyHash {
  std::size_t operator()(const OfrKey &k) const noexcept {
    /* Simple mix – good enough for this use case. */
    std::size_t h = static_cast<std::size_t>(k.trx_id);
    h ^= static_cast<std::size_t>(k.space_id) * 0xc4ceb9fe1a85ec53ULL;
    h ^= static_cast<std::size_t>(k.page_no)  * 0x517cc1b727220a95ULL;
    return h;
  }
};

/* ── registry value ──────────────────────────────────────────────── */
struct OfrEntry {
  uint64_t ofr_offset;       /*!< byte offset of image in storage file */
  uint32_t page_size;        /*!< page size (needed at recovery time)  */
  uint64_t meta_slot_offset; /*!< offset of this entry's slot in recovery_tmp.bin */
  uint8_t  storage_loc;      /*!< STORAGE_NVDIMM or STORAGE_SSD        */
};

/* ── main singleton ──────────────────────────────────────────────── */
/**
 * OfrRegistry
 *
 * Thread-safe registry tracking which (trx, page) combinations already have
 * a committed full-page image in opt-for-recovery.bin (NVDIMM) or
 * second-rcp.bin (SSD).
 *
 * The registry is keyed by (trx_id, space_id, page_no).  Multiple concurrent
 * transactions are handled via a shared_mutex – reads are lock-shared, writes
 * are lock-unique.
 */
class OfrRegistry {
 public:
  OfrRegistry() = default;
  ~OfrRegistry();

  /* ── lifecycle ─────────────────────────────────────────────────── */

  /** Open (or create) the OFR file and prepare the registry.
  @param[in] data_dir   InnoDB data directory (srv_data_home)
  @return true on success */
  bool open(const std::string &data_dir);

  /** Flush registry to recovery_tmp.bin and close all file handles.
      Called on clean shutdown AND from the signal/crash handler. */
  void close();

  /**
   * reset_meta_file
   *
   * Called by ofr_recover() after successful recovery.
   * Truncates recovery_tmp.bin to 0 and resets m_meta_write_offset so that
   * the new session appends entries starting from offset 0.
   * Must be called AFTER ofr_recover() reads and processes the crash entries.
   */
  void reset_meta_file();

  /* ── hot path (called on every page modification) ──────────────── */

  /**
   * capture_before_image
   *
   * Called BEFORE a page is modified by trx_id.  If no image for
   * (trx_id, page_id) exists yet in the OFR file the current committed
   * content of the page frame is written to the file and the entry is
   * registered.
   *
   * @param[in] trx_id   modifying transaction
   * @param[in] page_id  (space_id, page_no)
   * @param[in] frame    pointer to the 16 KiB (or whatever srv_page_size)
   *                     buffer frame – must be held under at least S-latch
   * @param[in] page_size physical page size in bytes
   * @return true if an image was written, false if already registered
   */
  bool capture_before_image(trx_id_t trx_id,
                                  const page_id_t &page_id,
                                  const byte *frame,
                                  uint32_t page_size);

  /* ── commit path ───────────────────────────────────────────────── */

  /**
   * on_trx_commit
   *
   * When a transaction commits its page images are no longer needed for undo
   * recovery.  Removes all entries for trx_id from the registry and marks the
   * corresponding slots in opt-for-recovery.bin as free (by zeroing the header
   * trx_id field – a zero trx_id is treated as "deleted" at recovery time).
   *
   * @param[in] trx_id   transaction that just committed
   */
  void on_trx_commit(trx_id_t trx_id);

  /* ── crash / recovery ──────────────────────────────────────────── */

  /**
   * flush_meta_to_disk
   *
   * No-op in the persistent-meta design: recovery_tmp.bin is kept
   * up-to-date on every ADD/DEL, so there is nothing left to flush
   * at crash time.  Kept for API compatibility.
   */
  void flush_meta_to_disk();

  /* ── accessors ─────────────────────────────────────────────────── */

  /** Return path to opt-for-recovery.bin */
  const std::string &ofr_path() const { return m_ofr_path; }

  /** Return path to recovery_tmp.bin */
  const std::string &meta_path() const { return m_meta_path; }

  /** Return path to second-rcp.bin (SSD) */
  const std::string &ssd_path() const { return m_ssd_path; }

  /** Total bytes appended to the OFR file so far. */
  uint64_t ofr_size() const {
    return m_ofr_write_offset.load(std::memory_order_relaxed);
  }

 private:
  /* Append one full-page record to opt-for-recovery.bin (NVDIMM).
     Returns the file offset at which the header was written, or UINT64_MAX
     on error. */
  uint64_t append_page_image(trx_id_t trx_id,
                              space_id_t space_id,
                              page_no_t page_no,
                              const byte *frame,
                              uint32_t page_size);

  /* Zero out the trx_id field in the NVDIMM OFR file for a written slot.
     SSD slots do not need invalidation (meta trx_id=0 is authoritative). */
  void invalidate_slot(uint64_t ofr_offset, uint8_t storage_loc = STORAGE_NVDIMM);

  /* Check if NVDIMM is above eviction threshold; if so, launch background
     eviction thread (non-blocking, no-op if already in progress).        */
  void trigger_eviction_if_needed();

  /* Background eviction: copy NVDIMM pages to SSD, update meta entries,
     then compact/reclaim NVDIMM.  Runs in a detached thread.             */
  void eviction_worker();

  /* ── data members ──────────────────────────────────────────────── */

  /** Shared mutex guarding m_map.
      Writers (maybe_capture / on_trx_commit) take unique lock.
      flush_meta_to_disk takes shared lock to avoid blocking readers long. */
  mutable std::shared_timed_mutex m_map_mutex;

  /** In-memory registry: (trx_id, space_id, page_no) → OfrEntry */
  std::unordered_map<OfrKey, OfrEntry, OfrKeyHash> m_map;

  /** Separate mutex serializing appends to the OFR file (NVDIMM).
      Lock order: m_map_mutex (unique) → m_file_mutex               */
  std::mutex m_file_mutex;

  /** File descriptor for opt-for-recovery.bin (-1 if not open). */
  int m_ofr_fd{-1};

  /** mmap base for opt-for-recovery.bin (nullptr if not mapped). */
  void *m_ofr_map{nullptr};

  /** Actual allocated (mmap'd) size of opt-for-recovery.bin.
      May be less than OFR_FILE_MAP_SIZE when NVDIMM capacity is limited. */
  size_t m_ofr_alloc_size{0};

  /** Monotonically increasing write offset into the OFR file.
      Updated atomically after each successful append. */
  std::atomic<uint64_t> m_ofr_write_offset{0};

  /* ── persistent meta file (recovery_tmp.bin) ──────────────────────────── */

  /** Mutex serializing appends and zero-outs to recovery_tmp.bin.
      Ordering rule: always acquired AFTER m_map_mutex to avoid deadlock. */
  std::mutex m_meta_file_mutex;

  /** File descriptor for recovery_tmp.bin (-1 if not open).
      Kept open for the lifetime of the registry so that every ADD/DEL
      is written immediately without re-opening the file. */
  int m_meta_fd{-1};

  /** mmap base for recovery_tmp.bin (nullptr if not mapped).
      Layout: [watermark:uint64_t][OfrMetaEntry...] */
  void *m_meta_map{nullptr};

  /** Monotonically increasing write offset into recovery_tmp.bin.
      Starts at OFR_META_HDR_SIZE (after watermark field).
      Each OfrMetaEntry occupies exactly sizeof(OfrMetaEntry) bytes. */
  std::atomic<uint64_t> m_meta_write_offset{OFR_META_HDR_SIZE};

  /* ── SSD second storage (second-rcp.bin) ─────────────────────────── */

  /** Mutex serializing appends to second-rcp.bin during eviction.
      Lock order: acquired independently of m_map_mutex.             */
  std::mutex m_ssd_file_mutex;

  /** File descriptor for second-rcp.bin (-1 if not open). */
  int m_ssd_fd{-1};

  /** Monotonically increasing write offset into second-rcp.bin. */
  std::atomic<uint64_t> m_ssd_write_offset{0};

  /* ── eviction state ───────────────────────────────────────────── */

  /** True while the background eviction thread is running.
      Prevents concurrent evictions; checked before launching thread. */
  std::atomic<bool> m_eviction_in_progress{false};

  /** Set to true on close() to signal the eviction thread to stop. */
  std::atomic<bool> m_shutdown{false};

  /** Handle for the background eviction thread (detached). */
  std::thread m_eviction_thread;

  std::string m_ofr_path;
  std::string m_meta_path;
  std::string m_ssd_path;

  bool m_open{false};
};

/* ── global singleton ────────────────────────────────────────────── */
extern OfrRegistry *ofr_registry;

/* Set to true when ofr_recover() succeeds during crash recovery.
   Used by srv_start() to skip the background undo rollback thread. */
extern bool ofr_recovery_done;

/**
 * ofr_current_trx_id
 *
 * Thread-local variable that holds the trx_id of the user transaction
 * currently executing on this thread.  It is set by trx_start_low() /
 * trx_set_rw_mode() when a trx_id is first assigned, and cleared to 0
 * by trx_commit_in_memory() at commit time.
 *
 * mtr_t::memo_modify_page() reads this value instead of requiring mtr_t
 * to carry a trx pointer, which keeps the mtr API minimal.
 *
 * Background / system mtrs that are not associated with a user transaction
 * will see 0 and correctly skip OFR capture.
 */
extern thread_local trx_id_t ofr_current_trx_id;

/* ── recovery entry point ────────────────────────────────────────── */

/**
 * ofr_recover
 *
 * Read recovery_tmp.bin + opt-for-recovery.bin (and second-rcp.bin if present)
 * and restore every uncommitted page to its last-committed state using
 * OFR_RECOVERY_THREADS worker threads and io_uring for async I/O.
 *
 * This replaces the classic row-level undo recovery for crash recovery.
 *
 * @param[in] data_dir  InnoDB data directory
 * @return true on success
 */
bool ofr_recover(const std::string &data_dir);

/**
 * ofr_crash_handler
 *
 * Must be called from any unclean-exit path (assertion failure, SIGABRT,
 * ut_error, etc.) BEFORE the process terminates.  Flushes the in-memory
 * OFR registry to recovery_tmp.bin so that the restart path can perform
 * full-page undo recovery.
 *
 * Safe to call even if ofr_registry is nullptr (no-op in that case).
 */
extern "C" void ofr_crash_handler();
