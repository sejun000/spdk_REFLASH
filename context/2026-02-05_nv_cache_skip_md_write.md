# NV Cache - Skip Metadata Writes During Runtime IO

## Date: 2026-02-05

## Goal
Runtime IO에서 nv_cache에 data write만 수행, metadata write는 모두 skip.
Power on/off (startup/shutdown) 시에는 기존 동작 유지. Recovery 불가해도 OK.

## Flag
- `bool skip_md_write` in `struct ftl_nv_cache` (ftl_nv_cache.h:148)
- Lifecycle: init=false → `ftl_nv_cache_resume()`: true → `ftl_nv_cache_halt()`: false

---

## Original Metadata Write Points (5개)

### 1. Tail metadata (P2L map) write at chunk close
- **File:** `lib/ftl/ftl_nv_cache.c`
- **Function:** `ftl_chunk_close()` → `ftl_chunk_basic_rq_write()`
- **What:** chunk의 P2L map (어떤 LBA가 어떤 offset에 있는지)을 chunk 끝 영역에 write
- **Impact:** `tail_md_chunk_blocks` blocks per chunk close
- **Callback chain:** `chunk_map_write_cb()` → (success) → persist CLOSED state (#3)
- **Skip 방식:** write_pointer/blocks_written만 advance하고, chunk_free_p2l_map 후 바로 finalize

### 2. Chunk OPEN state persist
- **File:** `lib/ftl/ftl_nv_cache.c`
- **Function:** `ftl_chunk_open()` → `ftl_md_persist_entries()`
- **What:** chunk metadata 영역에 state=OPEN을 persistent하게 기록
- **Impact:** 1 block per chunk open
- **Callback:** `chunk_open_cb()` → sets `chunk->md->state = FTL_CHUNK_STATE_OPEN`
- **Skip 방식:** `chunk->md->state = FTL_CHUNK_STATE_OPEN` 직접 설정 후 return

### 3. Chunk CLOSED state persist
- **File:** `lib/ftl/ftl_nv_cache.c`
- **Function:** `chunk_map_write_cb()` → `ftl_md_persist_entries()`
- **What:** tail md write 완료 후 chunk metadata 영역에 state=CLOSED + CRC 기록
- **Impact:** 1 block per chunk close
- **Callback:** `chunk_close_cb()` → free p2l_map, move to full list, set state
- **Skip 방식:** #1과 함께 skip됨 (ftl_chunk_close에서 직접 finalize)

### 4. Chunk FREE state persist
- **File:** `lib/ftl/ftl_nv_cache.c`
- **Function:** `ftl_chunk_persist_free_state()` → `ftl_md_persist_entries()`
- **What:** compaction 완료 후 chunk를 FREE로 만들 때 metadata에 state=FREE 기록
- **Impact:** 1 block per chunk free
- **Callback chain:** `chunk_free_cb()` → `spdk_bdev_unmap_blocks()` (TRIM) → `chunk_trim_cb()` → add to free list
- **Skip 방식:** persist 건너뛰고 바로 TRIM submit. chunk_free_persist_count 직접 decrement

### 5. P2L log write (non-VSS only)
- **File:** `lib/ftl/nvc/ftl_nvc_bdev_non_vss.c`
- **Function:** `write_io_cb()` → `ftl_p2l_log_io()`
- **What:** user IO 완료 후 P2L mapping을 별도 log 영역에 기록 (VSS가 없는 bdev용)
- **Impact:** per user IO (additional write)
- **Skip 방식:** `ftl_p2l_log_io` 대신 `ftl_nv_cache_write_complete(io, true)` 직접 호출

---

## Not Skipped

### VSS per-block metadata (ftl_nvc_bdev_vss.c)
- VSS metadata는 같은 NVMe write command에 inline으로 포함됨 (separate md buffer)
- 추가 write IO가 발생하지 않으므로 skip 불필요

---

## Modified Files
1. `lib/ftl/ftl_nv_cache.h` - skip_md_write flag 추가, resume에서 true 설정
2. `lib/ftl/ftl_nv_cache.c` - ftl_chunk_open, ftl_chunk_close, ftl_chunk_persist_free_state, ftl_nv_cache_halt
3. `lib/ftl/nvc/ftl_nvc_bdev_non_vss.c` - write_io_cb에서 P2L log skip

## Chunk Lifecycle Flow (Normal vs Skip)

### Normal:
```
FREE → ftl_chunk_open() → [persist OPEN] → OPEN
OPEN → ftl_chunk_close() → [write tail md] → [persist CLOSED] → CLOSED
CLOSED → compaction → ftl_chunk_free() → [persist FREE] → [TRIM] → FREE
```

### skip_md_write:
```
FREE → ftl_chunk_open() → [set OPEN directly] → OPEN
OPEN → ftl_chunk_close() → [advance wp, finalize directly] → CLOSED
CLOSED → compaction → ftl_chunk_free() → [TRIM directly] → FREE
```
