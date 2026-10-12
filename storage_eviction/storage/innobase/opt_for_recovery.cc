/*****************************************************************************
 * opt_for_recovery.cc
 *
 * Implementation of Optimised Full-Page Undo Recovery.
 * See include/opt_for_recovery.h for the full design description.
 *
 * Sections
 * ────────
 *  A. OfrRegistry – lifecycle (open / close)
 *  B. OfrRegistry – hot path  (capture_before_image)
 *  C. OfrRegistry – commit path (on_trx_commit)
 *  D. OfrRegistry – crash / recovery meta flush (flush_meta_to_disk)
 *  E. ofr_recover – multi-thread + io_uring recovery
 *
 *****************************************************************************/

#include "opt_for_recovery.h"

/* InnoDB checksum */
#include "buf0checksum.h"  /* buf_calc_page_crc32 */
#include "fil0types.h"     /* FIL_PAGE_SPACE_OR_CHKSUM, FIL_PAGE_END_LSN_OLD_CHKSUM */
#include "mach0data.h"     /* mach_write_to_4 */

/* POSIX / Linux */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* io_uring (Linux 5.1+) */
#ifdef HAVE_LIBURING
#  include <liburing.h>
#endif

/* C++ standard library */
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

/* InnoDB */
#include "fil0fil.h"      /* fil_space_get, fil_io  */
#include "page0size.h"    /* page_size_t, univ_page_size */
#include "srv0srv.h"      /* srv_data_home           */
#include "fil0types.h"    /* FIL_PAGE_LSN etc.       */
#include "ut0dbg.h"       /* ut_error                */
#include "fsp0fsp.h"      /* fsp_is_ibd_tablespace   */

OfrRegistry *ofr_registry = nullptr;

thread_local trx_id_t ofr_current_trx_id = 0;

bool ofr_recovery_done = false;

static bool pread_all(int fd, void *buf, size_t n, off_t off) {
  char *p = static_cast<char *>(buf);
  while (n > 0) {
    ssize_t r = ::pread(fd, p, n, off);
    if (r <= 0) {
      if (r < 0 && errno == EINTR) continue;
      return false;
    }
    p   += r;
    off += r;
    n   -= static_cast<size_t>(r);
  }
  return true;
}


OfrRegistry::~OfrRegistry() { close(); }

bool OfrRegistry::open(const std::string &data_dir) {
  /* NVDIMM files: DAX-mounted at /mnt/pmem, pwrite() is non-volatile.
   * SSD second storage: data_dir/second-rcp.bin (regular FS).           */
  m_ofr_path  = "/mnt/pmem/opt-for-recovery.bin";
  m_meta_path = "/mnt/pmem/recovery_tmp.bin";
  m_ssd_path  = data_dir + "/second-rcp.bin";

  /* ── opt-for-recovery.bin ─────────────────────────────────────────────── */
  m_ofr_fd = ::open(m_ofr_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (m_ofr_fd < 0) {
    fprintf(stderr, "Failed to open %s: %s\n",
            m_ofr_path.c_str(), strerror(errno));
    return false;
  }
  /* Preallocate OFR_FILE_MAP_SIZE (15 GB) on the NVDIMM.
   * DAX requires real fallocate – sparse ftruncate causes SIGBUS.
   * Skip re-allocation if a previous session already sized the file.    */
  {
    struct stat st{};
    const bool already_ok = (::fstat(m_ofr_fd, &st) == 0 &&
                              static_cast<size_t>(st.st_size) >= OFR_FILE_MAP_SIZE);
    if (!already_ok) {
      if (::fallocate(m_ofr_fd, 0, 0, static_cast<off_t>(OFR_FILE_MAP_SIZE)) != 0) {
        fprintf(stderr, "[OFR] fallocate ofr failed: %s\n", strerror(errno));
        ::close(m_ofr_fd);
        m_ofr_fd = -1;
        return false;
      }
    }
    m_ofr_alloc_size = OFR_FILE_MAP_SIZE;
  }
  m_ofr_map = ::mmap(nullptr, m_ofr_alloc_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, m_ofr_fd, 0);
  if (m_ofr_map == MAP_FAILED) {
    fprintf(stderr, "mmap ofr failed: %s\n", strerror(errno));
    m_ofr_map = nullptr;
  }
  /* Always start writing from offset 0 each session.
   * ofr_recover() reads old entries by their stored ofr_offset – it does
   * not depend on the write pointer.  After recovery the old data is no
   * longer needed and will be overwritten by the new session.            */
  m_ofr_write_offset.store(0, std::memory_order_relaxed);

  /* ── recovery_tmp.bin ─────────────────────────────────────────────────── *
   * Layout: [watermark:uint64_t][OfrMetaEntry...]                          *
   * watermark stores the absolute byte offset of the next write.           *
   * open() reads watermark to resume after crash entries; ofr_recover()    *
   * reads watermark to know how many slots to scan; reset_meta_file()      *
   * zeroes entries and resets watermark to OFR_META_HDR_SIZE.              */
  m_meta_fd = ::open(m_meta_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (m_meta_fd < 0) {
    fprintf(stderr, "Failed to open %s: %s\n",
            m_meta_path.c_str(), strerror(errno));
  } else {
    if (::fallocate(m_meta_fd, 0, 0, static_cast<off_t>(OFR_META_MAP_SIZE)) != 0) {
      fprintf(stderr, "[OFR] fallocate meta failed: %s\n", strerror(errno));
      ::close(m_meta_fd);
      m_meta_fd = -1;
      return false;
    }
    m_meta_map = ::mmap(nullptr, OFR_META_MAP_SIZE,
                        PROT_READ | PROT_WRITE, MAP_SHARED, m_meta_fd, 0);
    if (m_meta_map == MAP_FAILED) {
      fprintf(stderr, "mmap meta failed: %s\n", strerror(errno));
      m_meta_map = nullptr;
    }
    /* Read watermark to restore write position after crash */
    uint64_t watermark = OFR_META_HDR_SIZE;
    if (m_meta_map != nullptr) {
      memcpy(&watermark, m_meta_map, sizeof(uint64_t));
      if (watermark < OFR_META_HDR_SIZE) watermark = OFR_META_HDR_SIZE;
    }
    m_meta_write_offset.store(watermark, std::memory_order_relaxed);
  }

  /* ── second-rcp.bin (SSD) ──────────────────────────────────────────────── *
   * Opened with O_APPEND-like semantics via m_ssd_write_offset + pwrite().  *
   * No pre-allocation: file grows dynamically as pages are evicted.         */
  m_ssd_fd = ::open(m_ssd_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (m_ssd_fd < 0) {
    fprintf(stderr, "[OFR] Cannot open SSD file %s: %s (eviction disabled)\n",
            m_ssd_path.c_str(), strerror(errno));
    /* Non-fatal: eviction will be skipped if m_ssd_fd < 0 */
  } else {
    /* Resume from where a previous session left off (file may have data). */
    struct stat ssd_st{};
    if (::fstat(m_ssd_fd, &ssd_st) == 0 && ssd_st.st_size > 0) {
      m_ssd_write_offset.store(static_cast<uint64_t>(ssd_st.st_size),
                               std::memory_order_relaxed);
    } else {
      m_ssd_write_offset.store(0, std::memory_order_relaxed);
    }
  }

  m_shutdown.store(false, std::memory_order_relaxed);
  m_eviction_in_progress.store(false, std::memory_order_relaxed);

  m_open = true;
  fprintf(stderr, "[OFR] Opened (ofr_map=%p meta_map=%p meta_watermark=%" PRIu64
                  " ssd_fd=%d ssd_offset=%" PRIu64 ")\n",
          m_ofr_map, m_meta_map,
          m_meta_write_offset.load(std::memory_order_relaxed),
          m_ssd_fd,
          m_ssd_write_offset.load(std::memory_order_relaxed));
  return true;
}

void OfrRegistry::close() {
  if (!m_open) return;

  /* Signal eviction thread to stop and wait for it to finish. */
  m_shutdown.store(true, std::memory_order_release);
  if (m_eviction_thread.joinable()) m_eviction_thread.join();

  /* flush_meta_to_disk() is a no-op in the persistent-meta design;
     recovery_tmp.bin is already current.  Just sync and close. */
  if (m_meta_map != nullptr) {
    ::munmap(m_meta_map, OFR_META_MAP_SIZE);
    m_meta_map = nullptr;
  }
  if (m_meta_fd >= 0) {
    ::close(m_meta_fd);
    m_meta_fd = -1;
  }

  if (m_ofr_map != nullptr) {
    ::munmap(m_ofr_map, m_ofr_alloc_size);
    m_ofr_map = nullptr;
  }
  if (m_ofr_fd >= 0) {
    ::close(m_ofr_fd);
    m_ofr_fd = -1;
  }

  if (m_ssd_fd >= 0) {
    ::fdatasync(m_ssd_fd);
    ::close(m_ssd_fd);
    m_ssd_fd = -1;
  }

  m_open = false;
}

int validcnt = 0;
bool OfrRegistry::capture_before_image(trx_id_t trx_id,
                                              const page_id_t &page_id,
                                              const byte *frame,
                                              uint32_t page_size) {
  if (!m_open || m_ofr_map == nullptr) return false;
/*
     undo tablespace, system tablespace(ibdata1), temp tablespace except
     - undo page do not need
     - system page: do not need*/
  //Dohwan we capture only user data page(.ibd)
  if (!fsp_is_ibd_tablespace(page_id.space())) return false;

  const OfrKey key{trx_id, page_id.space(), page_id.page_no()};

  {
    std::shared_lock<std::shared_timed_mutex> sl(m_map_mutex);
    if (m_map.count(key)){
//	fprintf(stderr,"already exist page\n");
    	return false;
    }  
  }

  {
    std::unique_lock<std::shared_timed_mutex> ul(m_map_mutex);
    if (m_map.count(key)) return false;

    uint64_t off = append_page_image(trx_id,
                                     page_id.space(),
                                     page_id.page_no(),
                                     frame,
                                     page_size);
    if (off == UINT64_MAX) return false;

    /* ── Persistent meta write (ADD) ──────────────────────────────────────
     * Write OfrMetaEntry into the mmap'd recovery_tmp.bin region and
     * update the watermark so ofr_recover() knows how many slots to scan.  */
    uint64_t meta_off = UINT64_MAX;
    if (m_meta_map != nullptr) {
      OfrMetaEntry me{};
      me.trx_id      = static_cast<uint64_t>(trx_id);
      me.space_id    = page_id.space();
      me.page_no     = page_id.page_no();
      me.ofr_offset  = off;
      me.page_size   = page_size;
      me.storage_loc = STORAGE_NVDIMM;

      std::lock_guard<std::mutex> mlg(m_meta_file_mutex);
      meta_off = m_meta_write_offset.fetch_add(sizeof(OfrMetaEntry),
                                               std::memory_order_relaxed);
      memcpy(static_cast<char *>(m_meta_map) + meta_off, &me, sizeof(me));
      /* Update watermark then persist entry + watermark together.
       * Single ofr_persist covers the cache lines of both the entry
       * and the watermark at offset 0 (if they share a cache line the
       * CPU de-duplicates the flush automatically).                   */
      uint64_t new_watermark = meta_off + sizeof(OfrMetaEntry);
      memcpy(m_meta_map, &new_watermark, sizeof(uint64_t));
      ofr_persist(m_meta_map, new_watermark); /* [0..new_watermark) */
    }

    OfrEntry entry{off, page_size, meta_off, STORAGE_NVDIMM};
    m_map.emplace(key, entry);
  }
  validcnt++;
  trigger_eviction_if_needed();
  return true;
}
//Dohwan function of append page
uint64_t OfrRegistry::append_page_image(trx_id_t trx_id,
                                         space_id_t space_id,
                                         page_no_t page_no,
                                         const byte *frame,
                                         uint32_t page_size) {
  OfrPageHdr hdr{};
  hdr.trx_id    = static_cast<uint64_t>(trx_id);
  hdr.space_id  = space_id;
  hdr.page_no   = page_no;
  hdr.page_size = page_size;

  const size_t rec_size = sizeof(OfrPageHdr) + page_size;

  std::lock_guard<std::mutex> lg(m_file_mutex);

  uint64_t off = m_ofr_write_offset.fetch_add(rec_size,
                                               std::memory_order_relaxed);

  if (m_ofr_map == nullptr) {
    fprintf(stderr, "[OFR] ofr mmap not available\n");
    return UINT64_MAX;
  }
  if (off + rec_size > m_ofr_alloc_size) {
    fprintf(stderr, "[OFR] ofr file full (offset=%" PRIu64 " size=%zu alloc=%zu)\n",
            off, rec_size, m_ofr_alloc_size);
    return UINT64_MAX;
  }
  char *dst = static_cast<char *>(m_ofr_map) + off;

  /* Copy page data and recalculate checksum before storing.
   * Dirty pages in the InnoDB buffer pool have stale checksums
   * (checksum is only updated on flush to disk). If we capture a dirty
   * page as-is, recovery would write a page with mismatched checksum,
   * causing "page corruption" when InnoDB reads it back.               */
  byte *page_dst = reinterpret_cast<byte *>(dst + sizeof(OfrPageHdr));
  memcpy(page_dst, frame, page_size);

  /* Recalculate CRC32 checksum for the captured page data */
  uint32_t crc = buf_calc_page_crc32(page_dst);
  mach_write_to_4(page_dst,                             crc); /* FIL_PAGE_SPACE_OR_CHKSUM */
  mach_write_to_4(page_dst + page_size - FIL_PAGE_END_LSN_OLD_CHKSUM, crc); /* end checksum */

  ofr_persist(dst + sizeof(OfrPageHdr), page_size);

  /* Write header AFTER data so a crash before header → trx_id=0 → skip */
  memcpy(dst, &hdr, sizeof(hdr));
  ofr_persist(dst, sizeof(OfrPageHdr));

  return off;
}

void OfrRegistry::on_trx_commit(trx_id_t trx_id) {
  if (!m_open) return;

  struct CommitEntry { uint64_t ofr_offset; uint64_t meta_offset; uint8_t storage_loc; };
  std::vector<CommitEntry> to_invalidate;
  to_invalidate.reserve(16);

  {
    std::unique_lock<std::shared_timed_mutex> ul(m_map_mutex);
    for (auto it = m_map.begin(); it != m_map.end(); ) {
      if (it->first.trx_id == trx_id) {
        to_invalidate.push_back({it->second.ofr_offset,
                                  it->second.meta_slot_offset,
                                  it->second.storage_loc});
        it = m_map.erase(it);
      } else {
        ++it;
      }
    }
  }

  /* ── Persistent meta write (DEL) ──────────────────────────────────────
   * Zero out the trx_id field in each meta slot so recovery ignores it.  */
  if (m_meta_map != nullptr && !to_invalidate.empty()) {
    static const uint64_t zero_trx_id = 0;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (const auto &ce : to_invalidate) {
      uint64_t moff = ce.meta_offset;
      if (moff == UINT64_MAX) continue;
      memcpy(static_cast<char *>(m_meta_map) + moff,
             &zero_trx_id, sizeof(zero_trx_id));
      if (moff < lo) lo = moff;
      if (moff + sizeof(zero_trx_id) > hi) hi = moff + sizeof(zero_trx_id);
    }
    if (lo != UINT64_MAX) {
      ofr_persist(static_cast<char *>(m_meta_map) + lo, hi - lo);
    }
  }

  for (const auto &ce : to_invalidate) {
    invalidate_slot(ce.ofr_offset, ce.storage_loc);
  }
}

void OfrRegistry::invalidate_slot(uint64_t ofr_offset, uint8_t storage_loc) {
  /* SSD slots don't need per-slot invalidation: meta trx_id=0 is authoritative. */
  if (storage_loc == STORAGE_SSD) return;
  if (m_ofr_map == nullptr) return;
  static const uint64_t zero = 0;
  char *dst = static_cast<char *>(m_ofr_map) + ofr_offset;
  memcpy(dst, &zero, sizeof(zero));
  ofr_persist(dst, sizeof(zero));
}

/* flush_meta_to_disk
 * Called from the crash handler before process exit.
 * Single msync flushes all CPU-cached NVDIMM writes; fdatasync for SSD. */
void OfrRegistry::flush_meta_to_disk() {
  if (m_meta_map != nullptr) {
    uint64_t end = m_meta_write_offset.load(std::memory_order_relaxed);
    ::msync(m_meta_map, end, MS_SYNC);
  }
  if (m_ofr_map != nullptr) {
    uint64_t end = m_ofr_write_offset.load(std::memory_order_relaxed);
    if (end > 0)
      ::msync(m_ofr_map, end, MS_SYNC);
  }
  if (m_ssd_fd >= 0) {
    ::fdatasync(m_ssd_fd);
  }
}

/* reset_meta_file
 * Called by ofr_recover() after successful crash recovery.
 * Truncates recovery_tmp.bin to 0 bytes and resets m_meta_write_offset so
 * that normal-operation ADD entries start from offset 0.                    */
void OfrRegistry::reset_meta_file() {
  std::lock_guard<std::mutex> mlg(m_meta_file_mutex);
  if (m_meta_map != nullptr) {
    uint64_t old_end = m_meta_write_offset.load(std::memory_order_relaxed);
    /* Zero all entry data written this session so stale entries don't
     * confuse the next recovery scan.                                  */
    if (old_end > OFR_META_HDR_SIZE) {
      memset(static_cast<char *>(m_meta_map) + OFR_META_HDR_SIZE,
             0, old_end - OFR_META_HDR_SIZE);
    }
    /* Reset watermark to OFR_META_HDR_SIZE (empty, no entries) */
    uint64_t init_watermark = OFR_META_HDR_SIZE;
    memcpy(m_meta_map, &init_watermark, sizeof(uint64_t));
    /* Persist zeroed region + watermark */
    ofr_persist(m_meta_map, old_end);
    fprintf(stderr, "[OFR] reset_meta_file: recovery_tmp.bin reset\n");
  }
  m_meta_write_offset.store(OFR_META_HDR_SIZE, std::memory_order_relaxed);
}

//Dohwan multi + iouring phase
struct OfrRecoveryCtx {
  std::string                ofr_path;
  std::vector<OfrMetaEntry>  entries;

  std::atomic<uint32_t>      next_idx{0};

  std::atomic<uint32_t>      restored{0};

  std::atomic<uint32_t>      failed{0};
};


/* ── NVDIMM → SSD eviction ───────────────────────────────────────────────── */

void OfrRegistry::trigger_eviction_if_needed() {
  if (!m_open || m_ssd_fd < 0) return;
  if (m_eviction_in_progress.load(std::memory_order_relaxed)) return;

  uint64_t used      = m_ofr_write_offset.load(std::memory_order_relaxed);
  uint64_t threshold = static_cast<uint64_t>(m_ofr_alloc_size) *
                       OFR_EVICT_THRESHOLD / 100;
  if (used < threshold) return;

  bool expected = false;
  if (!m_eviction_in_progress.compare_exchange_strong(
          expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
    return;  /* another thread beat us */
  }

  fprintf(stderr, "[OFR-evict] Threshold hit (used=%" PRIu64 " threshold=%" PRIu64
                  ") – launching eviction thread\n", used, threshold);

  if (m_eviction_thread.joinable()) m_eviction_thread.join();
  m_eviction_thread = std::thread([this]() { eviction_worker(); });
}

void OfrRegistry::eviction_worker() {
  /* ── Step 1: Snapshot NVDIMM entries ──────────────────────────── */
  struct EvictItem {
    OfrKey   key;
    OfrEntry entry;
    uint64_t ssd_offset{UINT64_MAX};  /* filled after SSD write */
    bool     evicted{false};
  };

  uint64_t eviction_mark;
  std::vector<EvictItem> snapshot;

  {
    std::unique_lock<std::shared_timed_mutex> ul(m_map_mutex);
    eviction_mark = m_ofr_write_offset.load(std::memory_order_relaxed);
    for (const auto &[key, entry] : m_map) {
      if (entry.storage_loc == STORAGE_NVDIMM)
        snapshot.push_back({key, entry});
    }
  }

  if (snapshot.empty() || m_shutdown.load(std::memory_order_relaxed)) {
    m_eviction_in_progress.store(false, std::memory_order_release);
    return;
  }

  fprintf(stderr, "[OFR-evict] Evicting %zu NVDIMM pages (mark=%" PRIu64 ")\n",
          snapshot.size(), eviction_mark);

  /* ── Step 2: Write each page to SSD (pwrite, no mmap needed) ──── */
  for (auto &item : snapshot) {
    if (m_shutdown.load(std::memory_order_relaxed)) break;
    if (m_ofr_map == nullptr) break;

    uint32_t page_size = item.entry.page_size;
    size_t   rec_size  = sizeof(OfrPageHdr) + page_size;
    const char *src    = static_cast<const char *>(m_ofr_map) + item.entry.ofr_offset;

    uint64_t ssd_off;
    {
      std::lock_guard<std::mutex> lg(m_ssd_file_mutex);
      ssd_off = m_ssd_write_offset.fetch_add(rec_size, std::memory_order_relaxed);
    }

    ssize_t written = ::pwrite(m_ssd_fd, src, rec_size,
                                static_cast<off_t>(ssd_off));
    if (written < 0 || static_cast<size_t>(written) < rec_size) {
      fprintf(stderr, "[OFR-evict] SSD pwrite failed (space=%u page=%u): %s\n",
              item.key.space_id, item.key.page_no, strerror(errno));
      /* Roll back the offset reservation so the slot isn't lost */
      m_ssd_write_offset.fetch_sub(rec_size, std::memory_order_relaxed);
      continue;
    }

    item.ssd_offset = ssd_off;
    item.evicted    = true;
  }

  /* ── Step 3: fdatasync SSD once for all evicted pages ──────────── */
  size_t evicted_count = 0;
  for (const auto &item : snapshot) if (item.evicted) evicted_count++;

  if (evicted_count > 0) {
    ::fdatasync(m_ssd_fd);
    fprintf(stderr, "[OFR-evict] fdatasync done (%zu pages on SSD)\n", evicted_count);
  }

  /* ── Step 4: Update meta entries (storage_loc → SSD) then invalidate NVDIMM slots ── */
  for (auto &item : snapshot) {
    if (!item.evicted) continue;

    /* Update OfrMetaEntry: ofr_offset = SSD offset, storage_loc = STORAGE_SSD */
    if (item.entry.meta_slot_offset != UINT64_MAX && m_meta_map != nullptr) {
      char *meta_ptr = static_cast<char *>(m_meta_map) + item.entry.meta_slot_offset;
      memcpy(meta_ptr + offsetof(OfrMetaEntry, ofr_offset),
             &item.ssd_offset, sizeof(uint64_t));
      uint8_t loc = STORAGE_SSD;
      memcpy(meta_ptr + offsetof(OfrMetaEntry, storage_loc),
             &loc, sizeof(uint8_t));
      ofr_persist(meta_ptr, sizeof(OfrMetaEntry));
    }

    /* Update in-memory m_map */
    {
      std::unique_lock<std::shared_timed_mutex> ul(m_map_mutex);
      auto it = m_map.find(item.key);
      if (it != m_map.end()) {
        it->second.ofr_offset  = item.ssd_offset;
        it->second.storage_loc = STORAGE_SSD;
      }
    }

    /* Invalidate NVDIMM slot (trx_id → 0) */
    invalidate_slot(item.entry.ofr_offset, STORAGE_NVDIMM);
  }

  /* Persist meta updates to NVDIMM */
  if (m_meta_map != nullptr) {
    uint64_t meta_end = m_meta_write_offset.load(std::memory_order_relaxed);
    ::msync(m_meta_map, meta_end, MS_SYNC);
  }

  /* ── Step 5: Compact NVDIMM – move survivors to offset 0 ──────────
   * "Survivors" are NVDIMM entries written during eviction (offsets >= eviction_mark).
   * We memmove them to offset 0, update their ofr_offset, reset write pointer.
   * Lock order: m_map_mutex (unique) → m_file_mutex (pauses new appends).   */
  {
    std::unique_lock<std::shared_timed_mutex> map_ul(m_map_mutex);
    std::lock_guard<std::mutex> file_lg(m_file_mutex);

    uint64_t current_offset = m_ofr_write_offset.load(std::memory_order_relaxed);

    if (current_offset > eviction_mark && m_ofr_map != nullptr) {
      size_t  survivor_bytes = static_cast<size_t>(current_offset - eviction_mark);
      char   *src_base       = static_cast<char *>(m_ofr_map) + eviction_mark;
      char   *dst_base       = static_cast<char *>(m_ofr_map);

      memmove(dst_base, src_base, survivor_bytes);
      ofr_persist(dst_base, survivor_bytes);

      /* Update m_map and meta for compacted entries */
      for (auto &[key, entry] : m_map) {
        if (entry.storage_loc == STORAGE_NVDIMM && entry.ofr_offset >= eviction_mark) {
          uint64_t new_off = entry.ofr_offset - eviction_mark;

          if (entry.meta_slot_offset != UINT64_MAX && m_meta_map != nullptr) {
            char *meta_ptr = static_cast<char *>(m_meta_map) + entry.meta_slot_offset;
            memcpy(meta_ptr + offsetof(OfrMetaEntry, ofr_offset),
                   &new_off, sizeof(uint64_t));
          }
          entry.ofr_offset = new_off;
        }
      }

      m_ofr_write_offset.store(survivor_bytes, std::memory_order_relaxed);
      fprintf(stderr, "[OFR-evict] Compacted NVDIMM: %zu survivor bytes"
                      " (eviction_mark=%" PRIu64 ")\n",
              survivor_bytes, eviction_mark);
    } else {
      /* No survivors: simply reset write offset to 0 */
      m_ofr_write_offset.store(0, std::memory_order_relaxed);
      fprintf(stderr, "[OFR-evict] NVDIMM reclaimed (reset to 0)\n");
    }

    /* Persist meta after offset updates */
    if (m_meta_map != nullptr) {
      uint64_t meta_end = m_meta_write_offset.load(std::memory_order_relaxed);
      ::msync(m_meta_map, meta_end, MS_SYNC);
    }
  }

  m_eviction_in_progress.store(false, std::memory_order_release);
  fprintf(stderr, "[OFR-evict] Eviction complete (%zu evicted, mark=%" PRIu64 ")\n",
          evicted_count, eviction_mark);
}

/* ── recovery worker context ─────────────────────────────────────────────── */

/* forward declaration (defined below) */
static void ofr_worker_plain(OfrRecoveryCtx *ctx);

#ifdef HAVE_LIBURING
static void ofr_worker_uring(OfrRecoveryCtx *ctx) {
  fprintf(stderr, "iouring worker start\n");

  struct io_uring ring;

  /* SQPOLL 모드: 커널 polling thread가 SQ ring을 감시
   * → io_uring_submit() syscall 불필요, I/O 제출 오버헤드 제거.
   * Recovery는 대량 burst read이므로 SQPOLL 효과가 큼.
   * SQPOLL 초기화 실패 시 일반 모드(flags=0)로 fallback.       */
  /* Use default io_uring mode (no SQPOLL).
   * SQPOLL can cause data corruption when reading from DAX-mounted NVDIMM
   * because the kernel poll thread may access DAX memory incorrectly. */
  if (io_uring_queue_init(OFR_URING_QUEUE_DEPTH, &ring, 0) != 0) {
    fprintf(stderr, "[OFR-recover] io_uring_queue_init failed, falling back\n");
    ofr_worker_plain(ctx);
    return;
  }
  fprintf(stderr, "[OFR-recover] io_uring default mode enabled\n");

  int ofr_fd = ::open(ctx->ofr_path.c_str(), O_RDONLY);
  if (ofr_fd < 0) {
    fprintf(stderr, "[OFR-recover] Cannot open OFR file: %s\n", strerror(errno));
    io_uring_queue_exit(&ring);
    return;
  }

  /* ── 슬롯 구조 ──────────────────────────────────────────────────────────
   * 슬롯 수 = OFR_URING_QUEUE_DEPTH (64).
   * 슬롯마다 독립 버퍼를 가지므로 동시에 64개 read가 커널에서 진행 가능.
   * user_data = slot_id → CQE에서 어느 슬롯의 완료인지 식별.           */
  constexpr uint32_t MAX_PAGE = 65536;
  const uint32_t     DEPTH    = OFR_URING_QUEUE_DEPTH;

  /* Buffers must be 4096-byte aligned for InnoDB AIO (libaio / io_uring).
   * std::vector allocation is typically only 16-byte aligned, which causes
   * EINVAL when fil_io() uses Direct I/O internally.  Use posix_memalign. */
  struct Slot {
    uint8_t             *buf{nullptr};
    struct iovec         iov;
    uint32_t             entries_idx;
  };
  std::vector<Slot> slots(DEPTH);
  for (auto &s : slots) {
    if (::posix_memalign(reinterpret_cast<void **>(&s.buf), 4096, MAX_PAGE) != 0) {
      fprintf(stderr, "[OFR-recover] posix_memalign failed\n");
      io_uring_queue_exit(&ring);
      return;
    }
  }

  /* SQE 1개 준비: slot_id 슬롯에 entries[entries_idx] read 요청 등록 */
  auto submit_read = [&](uint32_t slot_id, uint32_t entries_idx) -> bool {
    const OfrMetaEntry &e = ctx->entries[entries_idx];
    Slot &s = slots[slot_id];
    s.entries_idx = entries_idx;
    s.iov.iov_base = s.buf;
    s.iov.iov_len  = e.page_size;

    const off_t data_off = static_cast<off_t>(e.ofr_offset + sizeof(OfrPageHdr));

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      io_uring_submit(&ring);
      sqe = io_uring_get_sqe(&ring);
      if (!sqe) return false;
    }
    io_uring_prep_readv(sqe, ofr_fd, &s.iov, 1, data_off);
    io_uring_sqe_set_data(sqe,
        reinterpret_cast<void *>(static_cast<uintptr_t>(slot_id)));
    return true;
  };

  /* ── Phase 1: ring를 꽉 채워 한 번에 submit (최대 64개) ──────────── */
  uint32_t inflight = 0;
  for (uint32_t slot_id = 0; slot_id < DEPTH; slot_id++) {
    uint32_t idx = ctx->next_idx.fetch_add(1, std::memory_order_relaxed);
    if (idx >= static_cast<uint32_t>(ctx->entries.size())) break;
    if (submit_read(slot_id, idx)) inflight++;
  }
  io_uring_submit(&ring);  /* 64개 한 번에 커널에 제출 */

  /* ── Phase 2: sliding window ─────────────────────────────────────────
   * CQE 도착 → fil_io(sync) 완료 → 같은 슬롯으로 다음 read submit.
   * fil_io 완료 후 슬롯 버퍼를 재사용하므로 커널 write와 race 없음.
   * fil_io 중에도 나머지 63개 슬롯의 read는 커널에서 비동기 진행.  */
  while (inflight > 0) {
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&ring, &cqe) < 0) {
      ctx->failed.fetch_add(1, std::memory_order_relaxed);
      inflight--;
      continue;
    }

    const uint32_t slot_id = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
    const int res = cqe->res;
    io_uring_cqe_seen(&ring, cqe);
    inflight--;

    /* 슬롯 재사용 전에 현재 항목 정보 복사 */
    const OfrMetaEntry e      = ctx->entries[slots[slot_id].entries_idx];
    uint8_t           *bufptr = slots[slot_id].buf;

    if (res < 0 || static_cast<uint32_t>(res) < e.page_size) {
      fprintf(stderr, "[OFR-recover] uring read failed space=%u page=%u: %s\n",
              e.space_id, e.page_no, strerror(-res));
      ctx->failed.fetch_add(1, std::memory_order_relaxed);
    } else {
      /* fil_io 동기 완료 → 이 시점까지 bufptr은 안전 */
      const page_id_t   page_id(e.space_id, e.page_no);
      const page_size_t page_size(e.page_size, e.page_size, false);
      dberr_t err = fil_io(IORequestWrite, true,
                           page_id, page_size, 0, e.page_size,
                           bufptr, nullptr);
      if (err != DB_SUCCESS) {
        fprintf(stderr, "[OFR-recover] fil_io write failed space=%u page=%u err=%d\n",
                e.space_id, e.page_no, static_cast<int>(err));
        ctx->failed.fetch_add(1, std::memory_order_relaxed);
      } else {
        ctx->restored.fetch_add(1, std::memory_order_relaxed);
      }
    }

    /* fil_io 완료 후 슬롯 재사용: 다음 페이지 read 제출 */
    uint32_t next_idx = ctx->next_idx.fetch_add(1, std::memory_order_relaxed);
    if (next_idx < static_cast<uint32_t>(ctx->entries.size())) {
      if (submit_read(slot_id, next_idx)) {
        io_uring_submit(&ring);
        inflight++;
      }
    }
  }

  ::close(ofr_fd);
  io_uring_queue_exit(&ring);
  for (auto &s : slots) free(s.buf);
}
#endif /* HAVE_LIBURING */
//Dohwan we dont use this func when iouring option is activated
static void ofr_worker_plain(OfrRecoveryCtx *ctx) {
  int ofr_fd = ::open(ctx->ofr_path.c_str(), O_RDONLY);
  if (ofr_fd < 0) {
    fprintf(stderr, "[OFR-recover] Cannot open OFR file: %s\n",
            strerror(errno));
    return;
  }

  uint8_t *page_buf = nullptr;
  if (::posix_memalign(reinterpret_cast<void **>(&page_buf), 4096, 65536) != 0) {
    fprintf(stderr, "[OFR-recover] posix_memalign failed\n");
    ::close(ofr_fd);
    return;
  }

  for (;;) {
    uint32_t idx = ctx->next_idx.fetch_add(1, std::memory_order_relaxed);
    if (idx >= static_cast<uint32_t>(ctx->entries.size())) break;

    const OfrMetaEntry &e = ctx->entries[idx];

    const off_t data_off =
        static_cast<off_t>(e.ofr_offset + sizeof(OfrPageHdr));

    if (!pread_all(ofr_fd, page_buf, e.page_size, data_off)) {
      fprintf(stderr,
              "[OFR-recover] pread failed space=%u page=%u: %s\n",
              e.space_id, e.page_no, strerror(errno));
      ctx->failed.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    const page_id_t   page_id(e.space_id, e.page_no);
    const page_size_t page_size(e.page_size, e.page_size, false);

    dberr_t err = fil_io(IORequestWrite, true,
                         page_id, page_size,
                         0, e.page_size,
                         page_buf, nullptr);

    if (err != DB_SUCCESS) {
      fprintf(stderr,
              "[OFR-recover] fil_io write failed space=%u page=%u err=%d\n",
              e.space_id, e.page_no, static_cast<int>(err));
      ctx->failed.fetch_add(1, std::memory_order_relaxed);
    } else {
      ctx->restored.fetch_add(1, std::memory_order_relaxed);
    }
  }

  ::close(ofr_fd);
  free(page_buf);
}
bool ofr_recover(const std::string &data_dir) {
  const std::string meta_path = "/mnt/pmem/recovery_tmp.bin";
  const std::string ofr_path  = "/mnt/pmem/opt-for-recovery.bin";
  const std::string ssd_path  = data_dir + "/second-rcp.bin";

  /* ── 1. Check files exist ─────────────────────────────────────────── */
  struct stat st{};
  if (::stat(meta_path.c_str(), &st) != 0) {
    fprintf(stderr,
            "[OFR-recover] %s not found – skipping OFR recovery\n",
            meta_path.c_str());
    return true; 
  }
  time_t now = time(NULL);
  struct tm tm_info;
  localtime_r(&now, &tm_info);

  char buf[64];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm_info);
  fprintf(stderr, "fullpage recovery logic start at  %s\n", buf); 

  /* ── 2. Read recovery_tmp.bin (mmap persistent-meta format) ──────────
   * Layout: [watermark:uint64_t][OfrMetaEntry...]
   * watermark = absolute byte offset of next write (>= OFR_META_HDR_SIZE).
   * Slots with trx_id == 0 were committed (DEL marker) and are skipped.  */
  int meta_fd = ::open(meta_path.c_str(), O_RDONLY);
  if (meta_fd < 0) {
    fprintf(stderr, "[recover] Cannot open %s: %s\n",
            meta_path.c_str(), strerror(errno));
    return false;
  }

  /* Read watermark from first 8 bytes */
  uint64_t watermark = OFR_META_HDR_SIZE;
  if (!pread_all(meta_fd, &watermark, sizeof(uint64_t), 0) ||
      watermark < OFR_META_HDR_SIZE) {
    watermark = OFR_META_HDR_SIZE;
  }

  const size_t entry_bytes = static_cast<size_t>(watermark - OFR_META_HDR_SIZE);
  const size_t total_slots = entry_bytes / sizeof(OfrMetaEntry);

  std::vector<OfrMetaEntry> all_slots(total_slots);
  if (total_slots > 0 &&
      !pread_all(meta_fd, all_slots.data(),
                 total_slots * sizeof(OfrMetaEntry),
                 static_cast<off_t>(OFR_META_HDR_SIZE))) {
    fprintf(stderr, "[recover] Failed to read meta entries\n");
    ::close(meta_fd);
    return false;
  }
  ::close(meta_fd);

  /* Filter: keep only valid entries (trx_id != 0 means uncommitted) */
  std::vector<OfrMetaEntry> entries;
  entries.reserve(total_slots);
  for (const auto &slot : all_slots) {
    if (slot.trx_id != 0) entries.push_back(slot);
  }

  {
    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm tm_info; localtime_r(&tv.tv_sec, &tm_info);
    char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
    fprintf(stderr, "[recover] Scanned %zu slots, %zu valid entries"
                    " from recovery_tmp.bin at %s.%06ld\n",
            total_slots, entries.size(), tbuf, (long)tv.tv_usec);
  }

  if (entries.empty()) {
    /* No uncommitted entries found (empty or already-committed file).
     * Return false so srv_dict_recover_on_restart() falls back to classic
     * undo recovery – needed when NVDIMM files are missing/empty but
     * uncommitted transactions still exist in the datadir. */
    fprintf(stderr, "[OFR-recover] No uncommitted entries – falling back to vanilla recovery\n");
    return false;
  }

  {
    std::unordered_map<uint64_t, size_t> page_map;
    page_map.reserve(entries.size());

    for (size_t i = 0; i < entries.size(); ++i) {
      uint64_t key = (static_cast<uint64_t>(entries[i].space_id) << 32)
                     | static_cast<uint64_t>(entries[i].page_no);
      auto it = page_map.find(key);
      if (it == page_map.end()) {
        page_map.emplace(key, i);
      } else if (entries[i].trx_id < entries[it->second].trx_id) {
        /* Earlier transaction (smaller trx_id) = more committed state.
         * Cross-storage offset comparison is meaningless since NVDIMM and
         * SSD offsets are in separate files; trx_id is the universal ordering. */
        it->second = i;
      }
    }

    std::vector<OfrMetaEntry> deduped;
    deduped.reserve(page_map.size());
    for (const auto &kv : page_map) {
      deduped.push_back(entries[kv.second]);
    }

    {
      struct timeval tv; gettimeofday(&tv, nullptr);
      struct tm tm_info; localtime_r(&tv.tv_sec, &tm_info);
      char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
      fprintf(stderr, "[OFR-recover] After dedup: %zu unique pages (dropped %zu"
              " overlapping entries) at %s.%06ld\n",
              deduped.size(), entries.size() - deduped.size(), tbuf, (long)tv.tv_usec);
    }

    entries = std::move(deduped);
  }

  /* ── Split entries by storage location ───────────────────────── */
  std::vector<OfrMetaEntry> nvdimm_entries, ssd_entries;
  for (const auto &e : entries) {
    if (e.storage_loc == STORAGE_SSD)
      ssd_entries.push_back(e);
    else
      nvdimm_entries.push_back(e);
  }
  fprintf(stderr, "[OFR-recover] NVDIMM pages=%zu  SSD pages=%zu\n",
          nvdimm_entries.size(), ssd_entries.size());

  /* ── Run recovery workers for NVDIMM and SSD in parallel ──────── */
  OfrRecoveryCtx ctx_nvdimm, ctx_ssd;
  ctx_nvdimm.ofr_path = ofr_path;
  ctx_nvdimm.entries  = std::move(nvdimm_entries);
  ctx_ssd.ofr_path    = ssd_path;
  ctx_ssd.entries     = std::move(ssd_entries);

  auto launch_workers = [&](OfrRecoveryCtx *ctx, const char *label) {
    if (ctx->entries.empty()) return;
    unsigned n = std::min(static_cast<unsigned>(OFR_RECOVERY_THREADS),
                          static_cast<unsigned>(ctx->entries.size()));
    {
      struct timeval tv; gettimeofday(&tv, nullptr);
      struct tm tmi; localtime_r(&tv.tv_sec, &tmi);
      char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmi);
      fprintf(stderr, "[OFR-recover] %s recovery started at %s.%06ld"
                      " (%u threads, %zu pages)\n",
              label, tbuf, (long)tv.tv_usec, n, ctx->entries.size());
    }
    std::vector<std::thread> workers;
    workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
#ifdef HAVE_LIBURING
      workers.emplace_back(ofr_worker_uring, ctx);
#else
      workers.emplace_back(ofr_worker_plain, ctx);
#endif
    }
    for (auto &t : workers) t.join();
    {
      struct timeval tv; gettimeofday(&tv, nullptr);
      struct tm tmi; localtime_r(&tv.tv_sec, &tmi);
      char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmi);
      fprintf(stderr, "[OFR-recover] %s done at %s.%06ld"
                      " restored=%u failed=%u\n",
              label, tbuf, (long)tv.tv_usec,
              ctx->restored.load(), ctx->failed.load());
    }
  };

  /* Launch NVDIMM and SSD recoveries in parallel */
  std::thread nvdimm_thread([&]() { launch_workers(&ctx_nvdimm, "NVDIMM"); });
  std::thread ssd_thread([&]()   { launch_workers(&ctx_ssd,    "SSD");    });
  nvdimm_thread.join();
  ssd_thread.join();

  uint32_t total_restored = ctx_nvdimm.restored.load() + ctx_ssd.restored.load();
  uint32_t total_failed   = ctx_nvdimm.failed.load()   + ctx_ssd.failed.load();

  {
    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm tm_info; localtime_r(&tv.tv_sec, &tm_info);
    char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
    fprintf(stderr, "[OFR-recover] All done – restored=%u  failed=%u at %s.%06ld\n",
            total_restored, total_failed, tbuf, (long)tv.tv_usec);
  }

  /* Do NOT unlink recovery_tmp.bin – it is a persistent mmap'd file on pmem.
   * reset_meta_file() zeros the written entries and resets the watermark so
   * the next session starts writing from offset OFR_META_HDR_SIZE.          */
  if (ofr_registry != nullptr) {
    ofr_registry->reset_meta_file();
  }

  return total_failed == 0;
}
