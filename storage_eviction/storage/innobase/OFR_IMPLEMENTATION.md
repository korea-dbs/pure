# OFR (Optimised Full-Page Recovery) 구현 정리

## 전체 흐름 요약

```
[트랜잭션 시작]
  trx_start_low() → ofr_current_trx_id = trx->id (thread_local)

[페이지 X/SX-latch 획득 시점 - committed 상태]
  buf_page_get_gen() → mtr_t::memo_push(block, MTR_MEMO_PAGE_X_FIX)
                     → ofr_try_capture(block)
                     → capture_before_image()
                         → fsp_is_ibd_tablespace() 필터 (user data만)
                         → m_map 중복 체크 (같은 trx+page는 1회만)
                         → append_page_image() → opt-for-recovery.bin 기록

[BUF_FIX → X/SX_FIX 승격 경로]
  mtr_t::x_latch_at_savepoint() / sx_latch_at_savepoint()
                     → ofr_try_capture(block)  (동일 경로)

[트랜잭션 커밋]
  trx_commit_in_memory() → on_trx_commit(trx_id)
                         → OFR 파일 해당 슬롯 trx_id=0 으로 invalidate
                         → 메모리 m_map 에서 제거

[Crash 발생]
  ut_dbg_assertion_failed / SIGABRT → ofr_crash_handler()
                                    → flush_meta_to_disk()
                                    → recovery_tmp.bin 에 {trx_id, space_id, page_no, ofr_offset} 기록

[재시작 Recovery]
  redo recovery 완료
  → srv_dict_recover_on_restart() → ofr_recover(data_dir)
       → recovery_tmp.bin 파싱
       → OFR 파일에서 trx_id≠0 슬롯만 필터링 (커밋된 항목 제외)
       → OFR_RECOVERY_THREADS 개 스레드 + io_uring 으로 fullpage 복원
       → fil_io() 로 테이블스페이스에 기록
```

---

## 수정된 파일 목록

| 파일 경로 | 수정 내용 |
|---|---|
| `include/mtr0mtr.ic` | `ofr_try_capture()` 추가, `memo_push()` / `x_latch_at_savepoint()` / `sx_latch_at_savepoint()` 에 캡처 삽입 |
| `include/mtr0log.ic` | `mlog_write_initial_log_record_fast()` 의 잘못된 OFR 캡처 코드 제거 및 주석 처리 |
| `mtr/mtr0mtr.cc` | `memo_modify_page()` 주석 업데이트 |
| `opt_for_recovery.cc` | `fsp0fsp.h` include 추가, `capture_before_image()` 에 tablespace 필터 추가 |

---

## 파일별 코드 상세 설명

---

### 1. `include/mtr0mtr.ic`

#### 1-1. includes (35~40번째 줄)

```cpp
#ifndef UNIV_HOTBACKUP
#include "opt_for_recovery.h"
#include "log0recv.h"
#include "srv0srv.h"  /* srv_page_size */
#endif
```

- `fsp0fsp.h` 는 **포함하지 않는다**.
- 이 파일은 `innochecksum` 같은 유틸리티에도 포함되는데,
  그 컨텍스트에서는 `fsp_is_ibd_tablespace` 가 선언되지 않아 컴파일 에러가 발생한다.
- tablespace 필터 로직은 `opt_for_recovery.cc` 내부로 이동했다.

---

#### 1-2. `ofr_try_capture()` (48~61번째 줄)

```cpp
static inline void ofr_try_capture(const buf_block_t *block) {
  if (ofr_registry != nullptr &&
      ofr_current_trx_id != 0 &&
      !recv_recovery_on) {
    const page_id_t page_id(block->page.id.space(),
                             block->page.id.page_no());
    ofr_registry->capture_before_image(
        ofr_current_trx_id,
        page_id,
        block->frame,
        static_cast<uint32_t>(srv_page_size));
  }
}
```

**guard 조건 3가지:**

| 조건 | 의미 |
|---|---|
| `ofr_registry != nullptr` | OFR 시스템이 초기화된 경우에만 |
| `ofr_current_trx_id != 0` | user transaction인 경우에만 (background/purge는 0) |
| `!recv_recovery_on` | redo replay 중에는 캡처 안 함 |

**왜 이 시점인가:**
- `block->frame` 은 X/SX-latch 획득 직후이므로 아직 아무것도 수정되지 않은 **committed 상태**이다.
- `mlog_write_*` 계열 함수들은 `mach_write_to_N()` 으로 페이지를 **먼저 수정한 뒤** redo log를 기록하므로, 그 경로에서는 이미 늦다.

---

#### 1-3. `mtr_t::memo_push()` 내 캡처 (88~103번째 줄)

```cpp
#ifndef UNIV_HOTBACKUP
  if (type == MTR_MEMO_PAGE_X_FIX || type == MTR_MEMO_PAGE_SX_FIX) {
    ofr_try_capture(reinterpret_cast<const buf_block_t *>(object));
  }
#endif
```

- 페이지가 **처음 X-latch 또는 SX-latch로 등록될 때** 캡처한다.
- S-latch (`MTR_MEMO_PAGE_S_FIX`) 는 read-only이므로 캡처하지 않는다.
- 커버되는 write 경로:
  - row update (clustered index)
  - row insert
  - secondary index update/insert
  - page split / merge
  - B-tree internal node update

---

#### 1-4. `mtr_t::sx_latch_at_savepoint()` 내 캡처 (166~172번째 줄)

```cpp
slot->type = MTR_MEMO_PAGE_SX_FIX;

/*
 * OFR: BUF_FIX → SX_FIX 승격 경로.
 * memo_push()는 BUF_FIX로 등록됐을 때 이미 호출됐지만 그 시점은
 * SX-latch가 없어서 캡처를 스킵했다. 승격 후 이 시점에 캡처한다.
 */
ofr_try_capture(block);
```

- 일부 경로에서 페이지가 `BUF_FIX` (latch 없음) 로 먼저 등록된 뒤,
  나중에 SX-latch 로 **승격** 되는 경우가 있다.
- 이 경우 `memo_push()` 시점에는 `type == MTR_MEMO_BUF_FIX` 여서 캡처를 스킵했으므로,
  승격되는 이 시점에 별도로 캡처한다.

---

#### 1-5. `mtr_t::x_latch_at_savepoint()` 내 캡처 (202~208번째 줄)

```cpp
slot->type = MTR_MEMO_PAGE_X_FIX;

/*
 * OFR: BUF_FIX → X_FIX 승격 경로.
 * memo_push()는 BUF_FIX로 등록됐을 때 이미 호출됐지만 그 시점은
 * X-latch가 없어서 캡처를 스킵했다. 승격 후 이 시점에 캡처한다.
 */
ofr_try_capture(block);
```

- `sx_latch_at_savepoint()` 와 동일한 이유. X-latch 승격 경로 처리.

---

### 2. `include/mtr0log.ic`

#### 2-1. `mlog_write_initial_log_record_fast()` 내 캡처 제거 (212~217번째 줄)

```cpp
/*
 * OFR: 이 시점은 mach_write_to_N() 이 이미 실행된 후이므로
 * before-image 캡처에 사용하지 않는다.
 * 캡처는 memo_push() / x_latch_at_savepoint() 에서 수행된다.
 * (include/mtr0mtr.ic 참조)
 */
```

**기존 코드가 잘못됐던 이유:**

`mlog_write_ulint(ptr, new_val, MLOG_4BYTES, mtr)` 의 실행 순서:

```
1. mach_write_to_4(ptr, new_val)   ← 페이지 버퍼에 새 값을 먼저 씀
2. mlog_write_initial_log_record_fast() 호출
3. 이 시점의 block->frame은 이미 new_val이 반영된 상태
```

따라서 이 시점에서 캡처하면 **before-image가 아니라 이미 수정된 상태**가 저장된다.
올바른 before-image는 X-latch 획득 직후인 `memo_push()` 에서 캡처해야 한다.

---

### 3. `opt_for_recovery.cc`

#### 3-1. includes (41~47번째 줄)

```cpp
#include "fil0fil.h"      /* fil_space_get, fil_io  */
#include "page0size.h"    /* page_size_t, univ_page_size */
#include "srv0srv.h"      /* srv_data_home           */
#include "fil0types.h"    /* FIL_PAGE_LSN etc.       */
#include "ut0dbg.h"       /* ut_error                */
#include "fsp0fsp.h"      /* fsp_is_ibd_tablespace   */  ← 추가
```

- `fsp_is_ibd_tablespace()` 는 `.cc` 파일에서만 include 한다.
- 헤더 파일(`.ic`, `.h`) 에서 include 하면 순환 의존성 및
  유틸리티 빌드 컨텍스트 에러가 발생한다.

---

#### 3-2. `capture_before_image()` - tablespace 필터 (156~160번째 줄)

```cpp
/* user data page(.ibd)만 캡처한다.
   undo tablespace, system tablespace(ibdata1), temp tablespace는 제외.
   - undo page : OFR recovery 경로에서 undo log를 사용하지 않으므로 불필요
   - system page: redo log로만 복구되며 여러 트랜잭션이 공유하므로 제외 */
if (!fsp_is_ibd_tablespace(page_id.space())) return false;
```

**왜 undo page와 system page를 제외하는가:**

| page 종류 | 제외 이유 |
|---|---|
| undo page | OFR recovery는 undo log를 전혀 사용하지 않으므로 캡처 불필요. 여러 트랜잭션이 같은 undo page를 공유하므로 before-image 복원 시 다른 committed trx의 undo record가 손상될 위험 있음 |
| system page (ibdata1) | redo log로만 복구됨. undo recovery 대상이 아님 |
| temp tablespace | 임시 데이터이므로 recovery 불필요 |

**`fsp_is_ibd_tablespace()` 내부 조건:**

```cpp
// fsp0fsp.h
return (space_id != TRX_SYS_SPACE          // system tablespace 제외 (space_id=0)
     && !fsp_is_undo_tablespace(space_id)   // undo tablespace 제외
     && !fsp_is_system_temporary(space_id));// temp tablespace 제외
```

---

#### 3-3. `capture_before_image()` 전체 흐름 (150~190번째 줄)

```
1. m_open && ofr_fd 유효성 체크
2. fsp_is_ibd_tablespace() 필터 → user data page만 통과
3. shared_lock으로 m_map에 (trx_id, space_id, page_no) 조회
   → 이미 있으면 return false (중복 방지 fast path)
4. unique_lock 재획득 후 double-check (race condition 방지)
5. append_page_image() → OfrPageHdr(20B) + raw page를 opt-for-recovery.bin에 pwrite
6. m_map에 {OfrKey → OfrEntry{offset, page_size}} 등록
```

---

#### 3-4. `on_trx_commit()` (230~255번째 줄)

```cpp
void OfrRegistry::on_trx_commit(trx_id_t trx_id) {
  // 1. unique_lock으로 m_map에서 해당 trx_id 항목 전부 수집 및 제거
  // 2. 수집한 각 offset에 대해 invalidate_slot() 호출
  //    → OFR 파일 해당 슬롯의 trx_id 필드를 0으로 덮어씀
}
```

- 커밋된 트랜잭션의 before-image는 recovery에 필요 없으므로 무효화한다.
- 파일에서 trx_id=0 이면 "커밋됨 / 복원 불필요" 로 판단한다.

---

#### 3-5. `flush_meta_to_disk()` (270~325번째 줄)

crash 발생 시 호출. 현재 m_map 의 모든 active 항목을 `recovery_tmp.bin` 에 기록.

```
recovery_tmp.bin 구조:
  OfrMetaHdr { magic, n_entries }
  OfrMetaEntry[] { trx_id, space_id, page_no, ofr_offset, page_size } × n
```

---

#### 3-6. `ofr_recover()` (497~607번째 줄)

재시작 시 redo recovery 완료 후 호출되는 진입점.

```
1. recovery_tmp.bin 파싱 → OfrMetaEntry[] 로드
2. OFR 파일 교차 검증:
   - 각 entry의 ofr_offset 에서 trx_id를 읽음
   - disk_trx_id == 0 이면 커밋된 것 → 필터링 제외
3. 남은 entry들 = 실제로 미커밋 상태에서 crash된 페이지들
4. OFR_RECOVERY_THREADS 개 worker thread 생성
   - io_uring 가용 시: ofr_worker_uring() 사용
   - fallback: ofr_worker_plain() 사용
5. 각 worker: opt-for-recovery.bin 에서 fullpage 읽기 → fil_io()로 테이블스페이스에 기록
6. recovery_tmp.bin 삭제
```

---

## 캡처 시점 비교 (핵심)

```
[잘못된 기존 방식] mlog_write_initial_log_record_fast()
  mach_write_to_N(ptr, new_val)   ← 페이지 수정 완료
  mlog_write_initial_log_record_fast() 에서 캡처
  → 이미 new_val이 반영된 상태 = before-image 아님

[올바른 현재 방식] memo_push() / x_latch_at_savepoint()
  buf_page_get_gen() → X-latch 획득
  memo_push(block, MTR_MEMO_PAGE_X_FIX) 에서 캡처
  → 아직 아무것도 수정되지 않은 committed 상태 = 진짜 before-image
  mach_write_to_N(ptr, new_val)   ← 캡처 이후에 수정
```

---

## 커버되는 write 경로

| write 경로 | `memo_push` 캡처 | 비고 |
|---|---|---|
| row update (clustered index) | ✅ | |
| row insert | ✅ | |
| secondary index update/insert | ✅ | |
| page split / merge | ✅ | 분리된 페이지도 각각 X-latch |
| FSP / system page | ❌ | `fsp_is_ibd_tablespace()` 필터로 제외 |
| undo page | ❌ | 동일 필터로 제외, OFR recovery에서 불필요 |
| temp tablespace | ❌ | 동일 필터로 제외 |
