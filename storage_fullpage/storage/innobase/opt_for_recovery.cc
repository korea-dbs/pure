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
#include "buf0buf.h"      /* buf_page_get, buf_block_get_frame */
#include "mtr0mtr.h"      /* mtr_start, mtr_commit   */
//Dohwan start
#include "page0page.h"    /* page_is_comp */
#include "rem0rec.h"      /* rec_set_deleted_flag_new/_old */
//Dohwan end

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
  /* Files are stored on NVDIMM (DAX-mounted at /mnt/pmem).
   * pwrite() goes directly to pmem — no fdatasync needed. */
  (void)data_dir;
  m_ofr_path   = "/mnt/pmem/opt-for-recovery.bin";
  m_meta_path  = "/mnt/pmem/recovery_tmp.bin";
  m_delta_path = "/mnt/pmem/delta-undo.bin";

  /* ── opt-for-recovery.bin ─────────────────────────────────────────────── */
  m_ofr_fd = ::open(m_ofr_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (m_ofr_fd < 0) {
    fprintf(stderr, "Failed to open %s: %s\n",
            m_ofr_path.c_str(), strerror(errno));
    return false;
  }
  /* Preallocate OFR_FILE_MAP_SIZE (10 GB) on the NVDIMM.
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

  /* ── delta-undo.bin (1 GB on NVDIMM) ────────────────────────────────── *
   * DAX requires real fallocate – sparse ftruncate causes SIGBUS.         */
  static constexpr size_t DELTA_MAP_SIZE = 3ULL * 1024 * 1024 * 1024; /* 3 GB */
  m_delta_fd = ::open(m_delta_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (m_delta_fd >= 0) {
    struct stat dst{};
    const bool delta_already_ok = (::fstat(m_delta_fd, &dst) == 0 &&
                                    (size_t)dst.st_size >= DELTA_MAP_SIZE);
    if (!delta_already_ok &&
        ::fallocate(m_delta_fd, 0, 0, static_cast<off_t>(DELTA_MAP_SIZE)) != 0) {
      fprintf(stderr, "[OFR] fallocate delta failed: %s\n", strerror(errno));
      ::close(m_delta_fd);
      m_delta_fd = -1;
    }
  }
  if (m_delta_fd >= 0) {
    m_delta_map = ::mmap(nullptr, DELTA_MAP_SIZE, PROT_READ | PROT_WRITE,
                         MAP_SHARED, m_delta_fd, 0);
    if (m_delta_map == MAP_FAILED) { m_delta_map = nullptr; }
    else {
      m_delta_alloc_size = DELTA_MAP_SIZE;
      /* Read existing watermark (first 8 bytes) */
      uint64_t wm = 0;
      memcpy(&wm, m_delta_map, sizeof(uint64_t));
      if (wm >= sizeof(uint64_t))
        m_delta_write_offset.store(wm, std::memory_order_relaxed);
      else
        m_delta_write_offset.store(sizeof(uint64_t), std::memory_order_relaxed);
    }
  }

  m_open = true;
  fprintf(stderr, "Opened (ofr_map=%p meta_map=%p delta_map=%p)\n",
          m_ofr_map, m_meta_map, m_delta_map);
  return true;
}

void OfrRegistry::close() {
  if (!m_open) return;

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

  if (m_delta_map != nullptr) {
    ::munmap(m_delta_map, m_delta_alloc_size);
    m_delta_map = nullptr;
  }
  if (m_delta_fd >= 0) {
    ::close(m_delta_fd);
    m_delta_fd = -1;
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
    if (m_delta_set.count(key)) return false; /* already in delta mode */

    /* ── Conflict detection: another uncommitted trx already has this page ──
     * If so, switch this (trx_id, page) to delta mode instead of capturing
     * a full physical before-image.  The physical before-image stored by the
     * earlier transaction is sufficient base; we store only row-level deltas
     * for this transaction via capture_row_delta().                          */
    bool conflict = false;
    for (const auto &kv : m_map) {
      if (kv.first.space_id == page_id.space() &&
          kv.first.page_no  == page_id.page_no()) {
        conflict = true;
        break;
      }
    }

    if (conflict) {
      /* Register as delta mode: write sentinel meta entry (ofr_offset=UINT64_MAX,
       * has_delta=1) so recovery knows to apply row deltas for this page+trx. */
      if (m_meta_map != nullptr) {
        OfrMetaEntry me{};
        me.trx_id     = static_cast<uint64_t>(trx_id);
        me.space_id   = page_id.space();
        me.page_no    = page_id.page_no();
        me.ofr_offset = UINT64_MAX;   /* sentinel: no physical image */
        me.page_size  = page_size;
        me.has_delta  = 1;

        std::lock_guard<std::mutex> mlg(m_meta_file_mutex);
        uint64_t meta_off = m_meta_write_offset.fetch_add(sizeof(OfrMetaEntry),
                                                          std::memory_order_relaxed);
        memcpy(static_cast<char *>(m_meta_map) + meta_off, &me, sizeof(me));
        uint64_t new_wm = meta_off + sizeof(OfrMetaEntry);
        memcpy(m_meta_map, &new_wm, sizeof(uint64_t));
        ofr_persist(m_meta_map, new_wm);
      }
      m_delta_set.insert(key);
      //fprintf(stderr, "[OFR] delta mode: space=%u page=%u trx=%llu\n",
      //        page_id.space(), page_id.page_no(), (unsigned long long)trx_id);
      return false; /* no full page captured */
    }

    uint64_t off = append_page_image(trx_id,
                                     page_id.space(),
                                     page_id.page_no(),
                                     frame,
                                     page_size);
    if (off == UINT64_MAX) return false;

    /* ── Persistent meta write (ADD) ──────────────────────────────────────*/
    uint64_t meta_off = UINT64_MAX;
    if (m_meta_map != nullptr) {
      OfrMetaEntry me{};
      me.trx_id     = static_cast<uint64_t>(trx_id);
      me.space_id   = page_id.space();
      me.page_no    = page_id.page_no();
      me.ofr_offset = off;
      me.page_size  = page_size;
      me.has_delta  = 0;

      std::lock_guard<std::mutex> mlg(m_meta_file_mutex);
      meta_off = m_meta_write_offset.fetch_add(sizeof(OfrMetaEntry),
                                               std::memory_order_relaxed);
      memcpy(static_cast<char *>(m_meta_map) + meta_off, &me, sizeof(me));
      uint64_t new_watermark = meta_off + sizeof(OfrMetaEntry);
      memcpy(m_meta_map, &new_watermark, sizeof(uint64_t));
      ofr_persist(m_meta_map, new_watermark);
    }

    OfrEntry entry{off, page_size, meta_off};
    m_map.emplace(key, entry);
  }
  validcnt ++;
 // fprintf(stderr,"valid fullpage write %d\n",validcnt);
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

//Dohwan if trx_commit make valid data to invalid
void OfrRegistry::on_trx_commit(trx_id_t trx_id) {
  if (!m_open) return;

  std::vector<uint64_t> offsets;       /* ofr_offset list (for invalidate_slot) */
  std::vector<uint64_t> meta_offsets;  /* meta_slot_offset list (for DEL marker) */
  offsets.reserve(16);
  meta_offsets.reserve(16);

  {
    std::unique_lock<std::shared_timed_mutex> ul(m_map_mutex);

    /* Remove physical before-image entries for committed trx */
    for (auto it = m_map.begin(); it != m_map.end(); ) {
      if (it->first.trx_id == trx_id) {
        offsets.push_back(it->second.ofr_offset);
        meta_offsets.push_back(it->second.meta_slot_offset);
        it = m_map.erase(it);
      } else {
        ++it;
      }
    }

    /* Remove delta-mode entries for committed trx.
     * Row deltas in delta-undo.bin are implicitly ignored at recovery because
     * we filter by trx_id (committed trx_id is not in recovery_tmp.bin). */
    for (auto it = m_delta_set.begin(); it != m_delta_set.end(); ) {
      if (it->trx_id == trx_id)
        it = m_delta_set.erase(it);
      else
        ++it;
    }
  }
  /* ── Persistent meta write (DEL) ──────────────────────────────────────
   * For each page belonging to the committed trx, zero out the trx_id
   * field in the meta slot so that recovery ignores this entry.
   * fdatasync ensures the invalidation survives a power failure.          */
  if (m_meta_map != nullptr && !meta_offsets.empty()) {
    static const uint64_t zero_trx_id = 0;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint64_t moff : meta_offsets) {
      if (moff == UINT64_MAX) continue;
      memcpy(static_cast<char *>(m_meta_map) + moff,
             &zero_trx_id, sizeof(zero_trx_id));
      if (moff < lo) lo = moff;
      if (moff + sizeof(zero_trx_id) > hi) hi = moff + sizeof(zero_trx_id);
    }
    /* Persist the full dirty range in one call */
    if (lo != UINT64_MAX) {
      ofr_persist(static_cast<char *>(m_meta_map) + lo, hi - lo);
    }
  }

  for (uint64_t off : offsets) {
    invalidate_slot(off);
  }
}
//Dohwan func of making invalid to fullpage.bin
void OfrRegistry::invalidate_slot(uint64_t ofr_offset) {
  if (m_ofr_map == nullptr) return;
  static const uint64_t zero = 0;
  char *dst = static_cast<char *>(m_ofr_map) + ofr_offset;
  memcpy(dst, &zero, sizeof(zero));
  ofr_persist(dst, sizeof(zero));
}

/* ── Delta undo: is_delta_page ──────────────────────────────────────────── */
bool OfrRegistry::is_delta_page(trx_id_t trx_id,
                                 uint32_t space_id,
                                 uint32_t page_no) const {
  const OfrKey key{trx_id, space_id, page_no};
  std::shared_lock<std::shared_timed_mutex> sl(m_map_mutex);
  return m_delta_set.count(key) > 0;
}

/* ── Delta undo: capture_row_delta ──────────────────────────────────────── */
void OfrRegistry::capture_row_delta(trx_id_t trx_id,
                                     uint32_t space_id,
                                     uint32_t page_no,
                                     uint16_t rec_offset,
                                     const byte *rec_data,
                                     uint16_t rec_len,
                                     uint8_t op) {
  if (!m_open || m_delta_map == nullptr) return;
  //Dohwan: OFR_DELTA_OP_INSERT carries no before-value, so rec_len==0 is
  //valid for that op; only reject an empty UPDATE delta.
  if (rec_len == 0 && op == OFR_DELTA_OP_UPDATE) return;

  const size_t total = sizeof(OfrDeltaEntry) + rec_len;

  std::lock_guard<std::mutex> lk(m_delta_file_mutex);
  uint64_t off = m_delta_write_offset.fetch_add(total, std::memory_order_relaxed);

  if (off + total > m_delta_alloc_size) {
    fprintf(stderr, "[OFR-delta] delta-undo.bin full, skipping\n");
    return;
  }

  OfrDeltaEntry hdr{};
  hdr.trx_id     = static_cast<uint64_t>(trx_id);
  hdr.space_id   = space_id;
  hdr.page_no    = page_no;
  hdr.rec_offset = rec_offset;
  hdr.rec_len    = rec_len;
  hdr.op         = op;
  hdr.pad[0] = hdr.pad[1] = hdr.pad[2] = 0;

  char *dst = static_cast<char *>(m_delta_map) + off;
  memcpy(dst, &hdr, sizeof(hdr));
  if (rec_len > 0) {
    memcpy(dst + sizeof(hdr), rec_data, rec_len);
  }

  /* Update watermark and persist */
  uint64_t new_wm = off + total;
  memcpy(m_delta_map, &new_wm, sizeof(uint64_t));
  ofr_persist(dst - off + 0, new_wm); /* persist [0..new_wm) */
}

//Dohwan start
/* ── Delta undo: capture_row_insert ───────────────────────────────────────
 * Thin wrapper: no before-value to save (rec_len=0), tagged
 * OFR_DELTA_OP_INSERT so recovery delete-marks instead of restoring bytes. */
void OfrRegistry::capture_row_insert(trx_id_t trx_id,
                                      uint32_t space_id,
                                      uint32_t page_no,
                                      uint16_t rec_offset) {
  capture_row_delta(trx_id, space_id, page_no, rec_offset,
                    nullptr, 0, OFR_DELTA_OP_INSERT);
}
//Dohwan end

/* flush_meta_to_disk
 * Called from the crash handler (ofr_crash_handler) before process exit.
 * Normal writes use ofr_persist=no-op for speed; this single msync at crash
 * time flushes all CPU-cached writes to NVDIMM so recovery can read them. */
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
  //Dohwan start
  /* delta-undo.bin was missing from the crash-time flush: ofr_persist()
   * is a no-op during normal operation, so row-delta writes (including the
   * watermark that gates how much of the file recovery will scan) could
   * sit in CPU cache only and never reach NVDIMM if the process dies here.
   * Flush it the same way as the other two files. */
  if (m_delta_map != nullptr) {
    uint64_t end = m_delta_write_offset.load(std::memory_order_relaxed);
    if (end > 0)
      ::msync(m_delta_map, end, MS_SYNC);
  }
  //Dohwan end
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

  /* Reset delta-undo.bin watermark so stale delta entries from prior sessions
   * are not re-applied during the next crash recovery scan.                  */
  if (m_delta_map != nullptr) {
    std::lock_guard<std::mutex> dlk(m_delta_file_mutex);
    uint64_t init_dwm = sizeof(uint64_t);
    memcpy(m_delta_map, &init_dwm, sizeof(uint64_t));
    ofr_persist(m_delta_map, sizeof(uint64_t));
    m_delta_write_offset.store(sizeof(uint64_t), std::memory_order_relaxed);
    fprintf(stderr, "[OFR] reset_meta_file: delta-undo.bin watermark reset\n");
  }
}

//Dohwan multi + iouring phase
struct OfrRecoveryCtx {
  std::string                ofr_path;
  std::vector<OfrMetaEntry>  entries;

  std::atomic<uint32_t>      next_idx{0};

  std::atomic<uint32_t>      restored{0};

  std::atomic<uint32_t>      failed{0};
};


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
//Dohwan core logic of fullpage recovery
bool ofr_recover(const std::string &data_dir) {
  (void)data_dir;
  const std::string meta_path = "/mnt/pmem/recovery_tmp.bin";
  const std::string ofr_path  = "/mnt/pmem/opt-for-recovery.bin";

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

  /* Filter: committed (trx_id==0) → skip.
   * has_delta==1 → row-delta undo path (no physical image to restore).
   * Others       → physical full-page restore (current OFR path).       */
  std::vector<OfrMetaEntry> entries;       /* physical restore entries */
  std::vector<OfrMetaEntry> delta_entries; /* row delta entries */
  entries.reserve(total_slots);
  delta_entries.reserve(16);
  for (const auto &slot : all_slots) {
    if (slot.trx_id == 0) continue;
    if (slot.has_delta == 1)
      delta_entries.push_back(slot);
    else
      entries.push_back(slot);
  }

  {
    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm tm_info; localtime_r(&tv.tv_sec, &tm_info);
    char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
    fprintf(stderr, "[recover] Scanned %zu slots, %zu physical, "
                    "%zu delta at %s.%06ld\n",
            total_slots, entries.size(), delta_entries.size(), tbuf, (long)tv.tv_usec);
  }

  if (entries.empty() && delta_entries.empty()) {
    fprintf(stderr, "[OFR-recover] No uncommitted entries – falling back to vanilla recovery\n");
    return false;
  }

  if (entries.empty()) {
    fprintf(stderr, "[OFR-recover] No physical entries, %zu delta-only – skipping to Phase 2\n",
            delta_entries.size());
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
      } else if (entries[i].ofr_offset < entries[it->second].ofr_offset) {
        it->second = i;  /* earlier capture = more committed state */
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
  OfrRecoveryCtx ctx;
  ctx.ofr_path = ofr_path;
  ctx.entries  = std::move(entries);

  const unsigned n_threads = std::min(
      static_cast<unsigned>(OFR_RECOVERY_THREADS),
      static_cast<unsigned>(ctx.entries.size()));
//Dphwan time of THD +iouring phase start
  {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_info);
    fprintf(stderr, "[OFR] iouring recovery started  at %s.%06ld"
                    " (%u threads, %zu pages)\n",
            buf, (long)tv.tv_usec, n_threads, ctx.entries.size());
  }

  std::vector<std::thread> workers;
  workers.reserve(n_threads);

  bool use_uring = false;
#ifdef HAVE_LIBURING
  {
    const char *env = getenv("OFR_SYNC_ONLY");
    use_uring = !(env && atoi(env) == 1);
  }
#endif
  fprintf(stderr, "[OFR-recover] I/O mode: %s\n",
          use_uring ? "io_uring (M+A)" : "sync pread (M)");

  for (unsigned i = 0; i < n_threads; ++i) {
#ifdef HAVE_LIBURING
    if (use_uring) {
      workers.emplace_back(ofr_worker_uring, &ctx);
      continue;
    }
#endif
    workers.emplace_back(ofr_worker_plain, &ctx);
  }

  for (auto &t : workers) t.join();

  /* iouring 종료 시각 (모든 스레드 join 이후) */
  {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_info);
    fprintf(stderr, "iouring recovery completed at %s.%06ld"
                    " restored=%u failed=%u\n",
            buf, (long)tv.tv_usec,
            ctx.restored.load(), ctx.failed.load());
  }

  {
    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm tm_info; localtime_r(&tv.tv_sec, &tm_info);
    char tbuf[32]; strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
    fprintf(stderr, "[OFR-recover] Done – restored=%u  failed=%u at %s.%06ld\n",
            ctx.restored.load(), ctx.failed.load(), tbuf, (long)tv.tv_usec);
  }

  /* ── Phase 2: apply row-delta undo for conflict pages ─────────────────
   * Read delta-undo.bin and apply before-values for uncommitted trx_ids
   * that are listed in delta_entries.                                      */
  if (!delta_entries.empty()) {
    /* Build set of (trx_id, space_id, page_no) needing delta undo */
    struct DeltaKey {
      uint64_t trx_id; uint32_t space_id; uint32_t page_no;
      bool operator==(const DeltaKey &o) const {
        return trx_id==o.trx_id && space_id==o.space_id && page_no==o.page_no;
      }
    };
    struct DeltaKeyHash {
      size_t operator()(const DeltaKey &k) const {
        size_t h = k.trx_id;
        h ^= (size_t)k.space_id * 0xc4ceb9fe1a85ec53ULL;
        h ^= (size_t)k.page_no  * 0x517cc1b727220a95ULL;
        return h;
      }
    };
    std::unordered_set<DeltaKey, DeltaKeyHash> delta_needed;
    for (const auto &de : delta_entries)
      delta_needed.insert({de.trx_id, de.space_id, de.page_no});

    /* Build (space_id, page_no) -> page_size lookup from delta_entries
       (which are OfrMetaEntry and carry page_size). */
    std::unordered_map<uint64_t, uint32_t> page_size_map;
    page_size_map.reserve(delta_entries.size());
    for (const auto &de : delta_entries) {
      uint64_t k = (static_cast<uint64_t>(de.space_id) << 32) |
                   static_cast<uint64_t>(de.page_no);
      page_size_map.emplace(k, de.page_size);
    }

    const std::string dpath = "/mnt/pmem/delta-undo.bin";
    int dfd = ::open(dpath.c_str(), O_RDONLY);
    if (dfd < 0) {
      fprintf(stderr, "[OFR-recover] delta-undo.bin not found, skipping delta undo\n");
    } else {
      uint64_t dwm = sizeof(uint64_t);
      pread_all(dfd, &dwm, sizeof(uint64_t), 0);

      /* Pass 1: Collect all needed delta entries from delta-undo.bin.
       * For each (trx_id, space_id, page_no, rec_offset) keep only the FIRST
       * captured entry (= before T2 ever touched the record).  Later captures
       * of the same slot are T2's intermediate states and must be discarded.  */
      struct SlotKey {
        uint64_t trx_id; uint32_t space_id; uint32_t page_no; uint16_t rec_offset;
        bool operator==(const SlotKey &o) const {
          return trx_id==o.trx_id && space_id==o.space_id &&
                 page_no==o.page_no && rec_offset==o.rec_offset;
        }
      };
      struct SlotKeyHash {
        size_t operator()(const SlotKey &k) const {
          size_t h = k.trx_id;
          h ^= (size_t)k.space_id  * 0xc4ceb9fe1a85ec53ULL;
          h ^= (size_t)k.page_no   * 0x517cc1b727220a95ULL;
          h ^= (size_t)k.rec_offset * 0x9e3779b97f4a7c15ULL;
          return h;
        }
      };
      /* PageKey: per-page patch list, (rec_offset → before-image bytes) */
      struct PageKey {
        uint32_t space_id; uint32_t page_no;
        bool operator==(const PageKey &o) const {
          return space_id==o.space_id && page_no==o.page_no;
        }
      };
      struct PageKeyHash {
        size_t operator()(const PageKey &k) const {
          return (size_t)k.space_id * 0xc4ceb9fe1a85ec53ULL ^
                 (size_t)k.page_no  * 0x517cc1b727220a95ULL;
        }
      };
      //Dohwan: op distinguishes UPDATE (restore data bytes) from
      //INSERT (delete-mark the record; data is empty)
      struct Patch { uint16_t rec_offset; uint8_t op; std::vector<byte> data; };
      std::unordered_map<PageKey, std::vector<Patch>, PageKeyHash> page_patches;
      std::unordered_set<SlotKey, SlotKeyHash> seen_slots;

      uint64_t scan_pos = sizeof(uint64_t);
      while (scan_pos + sizeof(OfrDeltaEntry) <= dwm) {
        OfrDeltaEntry hdr{};
        if (!pread_all(dfd, &hdr, sizeof(hdr), static_cast<off_t>(scan_pos))) break;
        scan_pos += sizeof(hdr);
        const size_t dlen = hdr.rec_len;
        if (scan_pos + dlen > dwm) break;

        DeltaKey dk{hdr.trx_id, hdr.space_id, hdr.page_no};
        if (delta_needed.count(dk)) {
          SlotKey sk{hdr.trx_id, hdr.space_id, hdr.page_no, hdr.rec_offset};
          if (seen_slots.insert(sk).second) {
            /* First capture for this slot: the true before-image
               (empty for OFR_DELTA_OP_INSERT, which carries no data). */
            std::vector<byte> buf(dlen);
            if (dlen > 0) {
              pread_all(dfd, buf.data(), dlen, static_cast<off_t>(scan_pos));
            }
            PageKey pk{hdr.space_id, hdr.page_no};
            page_patches[pk].push_back({hdr.rec_offset, hdr.op, std::move(buf)});
          }
          /* subsequent captures of same slot are intermediate states: skip */
        }
        scan_pos += dlen;
      }

      /* Pass 2: Apply patches page by page, fetching via the buffer pool
       * under X-latch instead of raw fil_io reads.
       *
       * We still write straight to disk via fil_io (not through the normal
       * dirty/flush-list path), but the X-latch is what actually matters
       * here: with OFR_RECOVERY_THREADS workers running concurrently, the
       * X-latch is what serializes any two workers that happen to land on
       * the same page (e.g. multiple patches from different (T,key) pairs
       * resolving to the same page), so the buffer-pool's own latch
       * mechanism gives us correct mutual exclusion instead of relying on
       * "pages never collide across workers" as an assumption.
       *
       * Parallelized across OFR_RECOVERY_THREADS workers, one page per
       * work item (same fan-out pattern as the Phase 1 io_uring workers). */
      struct DeltaPageWork {
        PageKey pk;
        std::vector<Patch> patches;
      };
      std::vector<DeltaPageWork> work_items;
      work_items.reserve(page_patches.size());
      for (auto &kv : page_patches) {
        work_items.push_back({kv.first, std::move(kv.second)});
      }

      std::atomic<size_t> next_idx{0};
      std::atomic<uint32_t> n_applied{0}, n_skipped{0};

      auto delta_worker = [&]() {
        for (;;) {
          const size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
          if (i >= work_items.size()) break;

          const PageKey &pk = work_items[i].pk;
          const std::vector<Patch> &patches = work_items[i].patches;

          uint64_t psz_key = (static_cast<uint64_t>(pk.space_id) << 32) |
                             static_cast<uint64_t>(pk.page_no);
          auto psz_it = page_size_map.find(psz_key);
          const uint32_t psz_bytes =
              (psz_it != page_size_map.end() && psz_it->second > 0)
              ? psz_it->second
              : static_cast<uint32_t>(UNIV_PAGE_SIZE);
          const page_size_t psz(psz_bytes, psz_bytes, false);
          const page_id_t pid(pk.space_id, pk.page_no);

          mtr_t mtr;
          mtr_start(&mtr);
          buf_block_t *block = buf_page_get(pid, psz, RW_X_LATCH, &mtr);
          if (block == nullptr) {
            mtr_commit(&mtr);
            n_skipped.fetch_add(static_cast<uint32_t>(patches.size()),
                                std::memory_order_relaxed);
            continue;
          }
          byte *frame = buf_block_get_frame(block);

          bool dirty = false;
          uint32_t local_applied = 0, local_skipped = 0;
          //Dohwan start
          const bool is_comp = page_is_comp(frame) != 0;
          //Dohwan end
          for (const auto &p : patches) {
            if (p.op == OFR_DELTA_OP_INSERT) {
              //Dohwan start
              /* Undo of an insert on a conflict page: delete-mark the
               * record instead of restoring bytes (there is nothing to
               * restore -- the record did not exist before). This is a
               * pure info-bits flip, so it needs neither dict_index_t
               * nor any PAGE_N_RECS/heap/directory bookkeeping; the
               * record is physically reclaimed later by normal purge,
               * same as any other delete-marked row. */
              if (p.rec_offset > 0 && p.rec_offset < psz.physical()) {
                rec_t *rec = reinterpret_cast<rec_t *>(frame + p.rec_offset);
                if (is_comp) {
                  rec_set_deleted_flag_new(rec, nullptr, TRUE);
                } else {
                  rec_set_deleted_flag_old(rec, TRUE);
                }
                dirty = true;
                local_applied++;
              } else {
                local_skipped++;
              }
              //Dohwan end
            } else if (p.rec_offset + p.data.size() <= psz.physical()) {
              memcpy(frame + p.rec_offset, p.data.data(), p.data.size());
              dirty = true;
              local_applied++;
            } else {
              local_skipped++;
            }
          }
          if (dirty) {
            /* Recompute CRC32 checksum once per page */
            const uint32_t csum = buf_calc_page_crc32(frame, false);
            mach_write_to_4(frame + FIL_PAGE_SPACE_OR_CHKSUM, csum);
            mach_write_to_4(frame + psz.physical() - FIL_PAGE_END_LSN_OLD_CHKSUM, csum);
            fil_io(IORequestWrite, true, pid, psz, 0, psz.physical(), frame, nullptr);
          }
          mtr_commit(&mtr);
          n_applied.fetch_add(local_applied, std::memory_order_relaxed);
          n_skipped.fetch_add(local_skipped, std::memory_order_relaxed);
        }
      };

      const unsigned n_delta_workers = static_cast<unsigned>(std::min<size_t>(
          OFR_RECOVERY_THREADS, std::max<size_t>(1, work_items.size())));
      std::vector<std::thread> delta_workers;
      delta_workers.reserve(n_delta_workers);
      for (unsigned i = 0; i < n_delta_workers; ++i) {
        delta_workers.emplace_back(delta_worker);
      }
      for (auto &t : delta_workers) t.join();

      ::close(dfd);
      fprintf(stderr, "[OFR-recover] delta undo: applied=%u skipped=%u (%u pages, %u workers)\n",
              n_applied.load(), n_skipped.load(),
              static_cast<unsigned>(work_items.size()), n_delta_workers);
    }
  }

  /* Do NOT unlink recovery_tmp.bin – it is a persistent mmap'd file on pmem.
   * reset_meta_file() zeros the written entries and resets the watermark so
   * the next session starts writing from offset OFR_META_HDR_SIZE.          */
  if (ofr_registry != nullptr) {
    ofr_registry->reset_meta_file();
  }

  return ctx.failed.load() == 0;
}
