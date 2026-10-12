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
  uint64_t trx_id;    /*!< owning transaction id; 0 = committed (DEL).
                           For has_delta=2 (OFR_META_REORG) this is NOT a
                           real trx_id -- it is set to OFR_META_REORG_TRX_ID
                           so the trx_id==0 "committed" filter never eats
                           this sentinel.                                    */
  uint32_t space_id;  /*!< tablespace id                                     */
  uint32_t page_no;   /*!< page number                                       */
  uint64_t ofr_offset;/*!< byte offset in opt-for-recovery.bin; UINT64_MAX
                           when has_delta=1 or has_delta=2 (no physical
                           image captured)                                   */
  uint32_t page_size; /*!< page size in bytes                                */
  uint32_t has_delta; /*!< 0 = physical entry, 1 = conflict page (row delta
                           undo), 2 = OFR_META_REORG sentinel: a page
                           reorganize happened on this page while it had
                           live delta tracking -- every delta entry for this
                           (space_id, page_no), from any trx, must be
                           recovered via PK re-search instead of by
                           rec_offset, since reorganize invalidates every
                           record's offset on the page at once.             */
};

//Dohwan start
/** Sentinel trx_id for has_delta==OFR_META_REORG meta entries. Must never
 * equal a real trx_id (which starts from small positive values) or 0 (the
 * "committed, skip" marker), so it is safe from both filters in the
 * recovery scan.                                                            */
static constexpr uint64_t OFR_META_REORG_TRX_ID = UINT64_MAX;
static constexpr uint32_t OFR_META_REORG        = 2;
//Dohwan end

//Dohwan start
/** OfrDeltaEntry::op values. */
static constexpr uint8_t OFR_DELTA_OP_UPDATE = 0; /*!< restore rec_len bytes
                                                       at rec_offset          */
static constexpr uint8_t OFR_DELTA_OP_INSERT = 1; /*!< delete-mark the record
                                                       at rec_offset (undo of
                                                       an insert on a
                                                       conflict page)         */

/** OfrDeltaEntry::is_sync values.
 * OFR_DELTA_ASYNC: the patch is a same-size, in-place byte overwrite
 *   (btr_cur_update_in_place only). rec_offset is stable for the lifetime
 *   of the page -- nothing else on the page can move it -- so recovery may
 *   patch the on-disk page directly (no buffer pool, no X-latch) and pages
 *   may be recovered fully independently of one another and of any
 *   OFR_DELTA_SYNC work on the same page.
 * OFR_DELTA_SYNC: the patch's rec_offset is only guaranteed valid if no
 *   intervening page reorganize happened (shrink-only variable-length
 *   update via btr_cur_optimistic_update, or an insert's delete-mark
 *   undo). Recovery must go through the buffer pool under RW_X_LATCH, and
 *   all SYNC patches for a page are applied by a single worker in-order,
 *   exactly like the pre-existing delta_worker path.                       */
static constexpr uint8_t OFR_DELTA_ASYNC = 0;
static constexpr uint8_t OFR_DELTA_SYNC  = 1;
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
  uint8_t  is_sync;    //Dohwan: OFR_DELTA_ASYNC or OFR_DELTA_SYNC
  uint8_t  pad[2];     /*!< padding for 8-byte alignment                    */
  //Dohwan start
  uint64_t roll_ptr;   /*!< this operation's own DB_ROLL_PTR, i.e. the exact
                           real undo log record this delta corresponds to.
                           Only meaningful (and only ever looked at) when
                           the owning page later ends up in reorg_pages at
                           recovery time -- see ofr_pending_vanilla /
                           ofr_run_pending_vanilla_rollback() in trx0roll.cc,
                           which replays exactly this record via the real
                           undo log instead of trusting rec_offset.        */
  //Dohwan end
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
   * @param roll_ptr    this operation's own DB_ROLL_PTR (as computed by
   *                    btr_cur_upd_lock_and_undo() / the insert-side
   *                    equivalent for this exact operation) -- the real
   *                    undo log record to fall back to if the page later
   *                    reorganizes and invalidates rec_offset. See
   *                    OfrDeltaEntry::roll_ptr.
   * @param op          OFR_DELTA_OP_UPDATE (default) or OFR_DELTA_OP_INSERT
   * @param is_sync     OFR_DELTA_ASYNC if rec_offset is stable for the life
   *                    of the page (same-size in-place update: nothing else
   *                    on the page can move it), OFR_DELTA_SYNC (default) if
   *                    a page reorganize could have invalidated rec_offset.
   *                    See OfrDeltaEntry::is_sync in opt_for_recovery.h.
   */
  void capture_row_delta(trx_id_t trx_id,
                         uint32_t space_id,
                         uint32_t page_no,
                         uint16_t rec_offset,
                         const byte *rec_data,
                         uint16_t rec_len,
                         roll_ptr_t roll_ptr,
                         uint8_t op = OFR_DELTA_OP_UPDATE,
                         uint8_t is_sync = OFR_DELTA_SYNC);

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
   * @param roll_ptr    this insert's own DB_ROLL_PTR -- see
   *                    capture_row_delta()'s roll_ptr parameter.
   */
  void capture_row_insert(trx_id_t trx_id,
                          uint32_t space_id,
                          uint32_t page_no,
                          uint16_t rec_offset,
                          roll_ptr_t roll_ptr);
  //Dohwan end

  /** Return true if (trx_id, space_id, page_no) is registered as delta mode. */
  bool is_delta_page(trx_id_t trx_id, uint32_t space_id, uint32_t page_no) const;

  //Dohwan start
  /**
   * is_delta_page_any
   *
   * Return true if (space_id, page_no) has a delta-mode entry for ANY
   * trx_id, not just one specific one. Used at the btr_page_reorganize_low()
   * hook, which does not know in advance which of the (possibly several)
   * transactions delta-tracking this page are affected -- it only needs to
   * know whether the page is worth marking at all.
   */
  bool is_delta_page_any(uint32_t space_id, uint32_t page_no) const;

  /**
   * mark_page_reorganized
   *
   * Called from the btr_page_reorganize_low() hook when
   * is_delta_page_any() is true for the page about to be rebuilt.
   * Reorganize reassigns every record's offset on the page at once, so
   * every delta entry captured for (space_id, page_no) so far -- from any
   * trx, not just the one whose operation triggered this reorganize --
   * becomes unsafe to replay via rec_offset. Writes an OFR_META_REORG
   * sentinel to recovery_tmp.bin so ofr_recover() routes every delta patch
   * for this page through PK re-search instead of a byte-range patch.
   * Idempotent: safe to call every time a delta-tracked page reorganizes,
   * duplicate sentinels just collapse into the same reorg_pages set entry
   * at recovery time.
   */
  void mark_page_reorganized(uint32_t space_id, uint32_t page_no);
  //Dohwan end

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

  //Dohwan start
  /** Map of (trx_id, space_id, page_no) in delta mode (conflict pages) to
      the byte offset of their has_delta=1 sentinel's trx_id field in
      recovery_tmp.bin. The offset (not just set membership) is needed so
      on_trx_commit() can zero that field out on commit -- exactly like
      m_map's meta_slot_offset does for full-page entries -- instead of
      only erasing the in-memory registration and leaving a permanently
      stale, real-trx_id sentinel for a transaction that already committed.
      Protected by m_map_mutex. */
  std::unordered_map<OfrKey, uint64_t, OfrKeyHash> m_delta_set;
  //Dohwan end

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

//delta write ratio
/** TPC-C-style workload instrumentation: how many page writes turn into
 * delta captures, broken down by kind, plus how many of those pages later
 * needed a reorganize. All relaxed atomics -- approximate counters for
 * offline analysis, not correctness-critical. Comment out this whole
 * block (search "delta write ratio" / "//end") to remove the feature. */
extern std::atomic<uint64_t> ofr_stat_page_writes;    /* every X/SX-latched
                                                          user page write
                                                          (mtr0mtr.ic) */
extern std::atomic<uint64_t> ofr_stat_delta_captures; /* capture_row_delta()
                                                          calls that actually
                                                          persisted a record */
extern std::atomic<uint64_t> ofr_stat_fixed;          /* op=UPDATE,
                                                          is_sync=ASYNC
                                                          (btr_cur_update_in_place) */
extern std::atomic<uint64_t> ofr_stat_shrink;         /* op=UPDATE,
                                                          is_sync=SYNC
                                                          (btr_cur_optimistic_update
                                                          shrink case) */
extern std::atomic<uint64_t> ofr_stat_insert;         /* op=INSERT */
extern std::atomic<uint64_t> ofr_stat_reorganize;     /* mark_page_reorganized()
                                                          calls */

/** Prints the counters above via fprintf(stderr, ...). Called from
 * OfrRegistry::close() on shutdown; safe to call any time. */
void ofr_print_delta_stats();
//end

//Dohwan start
/**
 * ofr_pending_vanilla_filter
 *
 * Thread-local, nullptr in every context except the dedicated OFR
 * pending-vanilla rollback pass (see trx0roll.cc / opt_for_recovery.cc).
 * When non-null, row_undo() (row0undo.cc) consults it before dispatching
 * to row_undo_ins()/row_undo_mod(): a record whose roll_ptr is NOT in the
 * set was already correctly restored by an OFR byte-range patch and is
 * skipped; a record whose roll_ptr IS in the set belongs to a page that
 * reorganized after OFR captured it, so it still needs real (vanilla)
 * undo and is applied normally.
 *
 * Set/cleared only around the call to trx_rollback_active() for one
 * specific resurrected trx inside the pending-vanilla driver -- never
 * left set for a normal ROLLBACK statement or ordinary crash recovery.
 */
extern thread_local const std::unordered_set<roll_ptr_t> *ofr_pending_vanilla_filter;

/**
 * ofr_pending_vanilla
 *
 * trx_id -> {roll_ptr, ...} of delta records that OFR captured but could
 * NOT safely replay as a byte-range patch, because the page they live on
 * reorganized at some point after capture (see mark_page_reorganized() /
 * OFR_META_REORG). Populated by ofr_recover()'s Phase 2 when it routes a
 * page's patches away from the sync/async buckets because the page is in
 * reorg_pages; every (trx_id, roll_ptr) routed there lands here instead of
 * being silently dropped.
 *
 * Consumed later, once the data dictionary is guaranteed ready (near
 * srv_start_threads(), NOT inside the early ofr_recover() call -- see
 * ofr_run_pending_vanilla_rollback() in trx0roll.cc), by walking each
 * named trx's real undo chain and selectively re-applying only the named
 * records via the existing row_undo_ins()/row_undo_mod() machinery.
 *
 * Left empty (the common case: no reorganize happened on any delta page)
 * this whole pass is skipped entirely, so it costs nothing when unused.
 */
extern std::unordered_map<trx_id_t, std::unordered_set<roll_ptr_t>> ofr_pending_vanilla;
//Dohwan end

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
