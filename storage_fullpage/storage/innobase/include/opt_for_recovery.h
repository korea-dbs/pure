 /* opt_for_recovery.h
 * Optimized Crash Recovery via Full-Page Before-Images.
 * Design overview
 * Before a transaction modifies a page for the first time (since the page was
 * last in a committed state) the engine captures the FULL, committed page
 * image and appends it to  opt-for-recovery.bin  (OFR file).
 * In-memory registry (OfrRegistry)
 *   key  : (trx_id, space_id, page_no)
 *   value: byte offset inside opt-for-recovery.bin where the image lives
 *****************************************************************************/

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
static constexpr size_t   OFR_META_MAP_SIZE = 1ULL * 1024 * 1024 * 1024;  /* 1 GB   */
static constexpr size_t   OFR_FILE_MAP_SIZE = 10ULL * 1024 * 1024 * 1024; /* 10 GB  */
static constexpr uint64_t OFR_META_HDR_SIZE = sizeof(uint64_t); /* watermark field */

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
  uint64_t trx_id;    /*!< owning transaction id; 0 = committed (DEL)       */
  uint32_t space_id;  /*!< tablespace id                                     */
  uint32_t page_no;   /*!< page number                                       */
  uint64_t ofr_offset;/*!< byte offset in opt-for-recovery.bin; UINT64_MAX
                           when has_delta=1 (no physical image captured)     */
  uint32_t page_size; /*!< page size in bytes                                */
  uint32_t has_delta; /*!< 1 = conflict page, use row delta undo instead     */
};

//Dohwan start
/** OfrDeltaEntry::op values. */
static constexpr uint8_t OFR_DELTA_OP_UPDATE = 0; /*!< restore rec_len bytes
                                                       at rec_offset          */
static constexpr uint8_t OFR_DELTA_OP_INSERT = 1; /*!< delete-mark the record
                                                       at rec_offset (undo of
                                                       an insert on a
                                                       conflict page)         */
//Dohwan end

/** On-disk header for each row-delta record in delta-undo.bin.
    Immediately followed by `rec_len` bytes of before-value data
    (OFR_DELTA_OP_UPDATE only; empty for OFR_DELTA_OP_INSERT). */
struct OfrDeltaEntry {
  uint64_t trx_id;     /*!< transaction that owns this delta                */
  uint32_t space_id;   /*!< tablespace id                                   */
  uint32_t page_no;    /*!< page number                                     */
  uint16_t rec_offset; /*!< byte offset of the record inside the page       */
  uint16_t rec_len;    /*!< byte length of before-value that follows        */
  uint8_t  op;         //Dohwan: OFR_DELTA_OP_UPDATE or OFR_DELTA_OP_INSERT
  uint8_t  pad[3];     /*!< padding for 8-byte alignment                    */
  /* rec_len bytes of before-value immediately follow this struct */
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
  uint64_t ofr_offset;      /*!< byte offset of the image in opt-for-recovery.bin */
  uint32_t page_size;       /*!< page size (needed at recovery time)               */
  uint64_t meta_slot_offset;/*!< byte offset of this entry's slot in recovery_tmp.bin
                                 used to zero-out trx_id on commit (DEL marker)    */
};

/* ── main singleton ──────────────────────────────────────────────── */
/**
 * OfrRegistry
 *
 * Thread-safe registry tracking which (trx, page) combinations already have
 * a committed full-page image in opt-for-recovery.bin.
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

  /**
   * capture_row_delta
   *
   * Called from btr_cur_update_in_place BEFORE the record is modified.
   * Stores the before-value of the record to delta-undo.bin on NVDIMM.
   * Only called when this (trx_id, page) is in delta mode (conflict page).
   *
   * @param trx_id      modifying transaction
   * @param space_id    tablespace
   * @param page_no     page number
   * @param rec_offset  offset of record inside page
   * @param rec_data    pointer to current (before-modification) record bytes
   * @param rec_len     length of record in bytes
   * @param op          OFR_DELTA_OP_UPDATE (default) or OFR_DELTA_OP_INSERT
   */
  void capture_row_delta(trx_id_t trx_id,
                         uint32_t space_id,
                         uint32_t page_no,
                         uint16_t rec_offset,
                         const byte *rec_data,
                         uint16_t rec_len,
                         uint8_t op = OFR_DELTA_OP_UPDATE);

  //Dohwan start
  /**
   * capture_row_insert
   *
   * Called from btr_cur_optimistic_insert / btr_cur_pessimistic_insert
   * AFTER the record has been written, only when this (trx_id, page) is
   * in delta mode (conflict page). Unlike capture_row_delta, there is no
   * "before value" to save -- the record did not exist before. Recovery
   * undoes this by delete-marking (not physically removing) the record
   * at rec_offset if trx_id turns out to be uncommitted, matching how
   * DELETE-undo already works: a pure in-place bit flip that needs no
   * page-structure bookkeeping (PAGE_N_RECS, directory slots, etc.) and
   * no dict_index_t at recovery time. The record is physically reclaimed
   * later by the normal purge thread, same as any other delete-marked row.
   *
   * @param trx_id      inserting transaction
   * @param space_id    tablespace
   * @param page_no     page number
   * @param rec_offset  offset of the newly inserted record inside the page
   */
  void capture_row_insert(trx_id_t trx_id,
                          uint32_t space_id,
                          uint32_t page_no,
                          uint16_t rec_offset);
  //Dohwan end

  /** Return true if (trx_id, space_id, page_no) is registered as delta mode. */
  bool is_delta_page(trx_id_t trx_id, uint32_t space_id, uint32_t page_no) const;

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

  /** Total bytes appended to the OFR file so far. */
  uint64_t ofr_size() const {
    return m_ofr_write_offset.load(std::memory_order_relaxed);
  }

 private:
  /* Append one full-page record to opt-for-recovery.bin.
     Returns the file offset at which the header was written, or UINT64_MAX
     on error. */
  uint64_t append_page_image(trx_id_t trx_id,
                              space_id_t space_id,
                              page_no_t page_no,
                              const byte *frame,
                              uint32_t page_size);

  /* Zero out the trx_id field in the OFR file for an already-written slot
     (marks it as "committed / deleted"). */
  void invalidate_slot(uint64_t ofr_offset);

  /* ── data members ──────────────────────────────────────────────── */

  /** Shared mutex guarding m_map.
      Writers (maybe_capture / on_trx_commit) take unique lock.
      flush_meta_to_disk takes shared lock to avoid blocking readers long. */
  mutable std::shared_timed_mutex m_map_mutex;

  /** In-memory registry: (trx_id, space_id, page_no) → OfrEntry */
  std::unordered_map<OfrKey, OfrEntry, OfrKeyHash> m_map;

  /** Set of (trx_id, space_id, page_no) in delta mode (conflict pages).
      Protected by m_map_mutex. */
  std::unordered_set<OfrKey, OfrKeyHash> m_delta_set;

  /** Separate mutex serializing appends to the OFR file. */
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

  /* ── delta-undo.bin (row-level before-values for conflict pages) ─────── */
  std::mutex  m_delta_file_mutex;
  int         m_delta_fd{-1};
  void       *m_delta_map{nullptr};
  size_t      m_delta_alloc_size{0};
  std::atomic<uint64_t> m_delta_write_offset{sizeof(uint64_t)}; /* starts after watermark */

  std::string m_ofr_path;
  std::string m_meta_path;
  std::string m_delta_path;

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
 * Read recovery_tmp.bin + opt-for-recovery.bin and restore every uncommitted
 * page to its last-committed state using OFR_RECOVERY_THREADS worker threads
 * and io_uring for async I/O.
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
