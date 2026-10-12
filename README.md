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
