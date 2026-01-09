# Duplicate Segment in free_pool Bug Fix

## Date: 2026-01-09

## Symptoms
- fio verify 중 간헐적으로 NVMe READ error 발생
- `get_cache_location: key=X but block.key=Y MISMATCH!` 에러
- `BUG! complete_segment_reset: seg=... ALREADY IN free_pool!` 에러

## Root Cause Analysis

### 1. Missing `evict_in_progress` flag in chunks.empty() case

`start_gc_or_evict`에서 `evict_result.chunks.empty() && evict_result.victim_seg` 케이스:
- valid block이 없어서 바로 `reset_segment_async` 호출
- **`evict_in_progress` 플래그를 설정하지 않음!**
- 결과: 다른 `start_gc_or_evict` 호출이 동시에 진행될 수 있음

### 2. Duplicate add in CbEvictPolicy

`CbEvictPolicy::add`에서 이미 heap에 있는 segment를 다시 add할 때:
```cpp
void CbEvictPolicy::add(Segment* s)
{
    auto h = heap_.push({ score(s), s });
    h_[s] = h;  // 기존 handle을 덮어씀!
}
```

**문제 시나리오:**
1. 첫 번째 `add(seg)` → heap에 추가, `h_[seg] = handle1`
2. 두 번째 `add(seg)` → heap에 또 추가, `h_[seg] = handle2` (handle1 덮어씀)
3. `remove(seg)` → `handle2`만 제거, **handle1은 heap에 남아있음!**
4. `choose_segment()` → handle1이 선택됨 (이미 free_pool에 있는 segment!)

### 3. Resulting Bug

같은 segment가:
1. free_pool에 추가됨
2. evictor에서 다시 victim으로 선택됨 (handle1이 남아있어서)
3. `reset_segment_async` 다시 호출
4. `complete_segment_reset`에서 이미 free_pool에 있는 segment 감지

## Fixes

### Fix 1: Set evict_in_progress in chunks.empty() case
**File:** `log_cache_wrapper.cpp`

```cpp
// Before
cache->cache()->reset_segment_async(evict_result.victim_seg, simple_reset_done, reset_ctx);

// After
reset_ctx->cache = cache;
cache->set_evict_in_progress(true);
cache->cache()->reset_segment_async(evict_result.victim_seg, simple_reset_done, reset_ctx);
```

`simple_reset_done` 콜백에서 `evict_in_progress = false` 해제.

### Fix 2: Add duplicate check in CbEvictPolicy::add
**File:** `evict_policy_cost_benefit.cpp`

```cpp
void CbEvictPolicy::add(Segment* s)
{
    assert(s);
    if (h_.count(s)) return;  // Already in heap, skip duplicate add
    auto h = heap_.push({ score(s), s });
    h_[s] = h;
}
```

## Files Modified
- `/home/sejun000/spdk_25/module/bdev/icache/logcache/log_cache_wrapper.cpp`
  - `SimpleResetCtx` 구조체에 `cache` 포인터 추가
  - `simple_reset_done`에서 `evict_in_progress` 해제
  - `chunks.empty()` 케이스에서 `evict_in_progress` 설정

- `/home/sejun000/spdk_25/module/bdev/icache/logcache/port/evict_policy_cost_benefit.cpp`
  - `add`에 중복 체크 추가

## Testing
- fio verify 테스트 통과
- 성능 안정적
