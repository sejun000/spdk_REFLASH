# 2026-01-10: FDP Stats Logger Fix & Stream Classification Bug Fix

## 1. FDP Statistics Log Page Fix

### Problem
- NVMe FDP stats (HBMW, MBMW)가 항상 0으로 출력됨
- icache stats logger에서 NVMe log page 읽기가 실패

### Root Cause
1. **구조체 크기 오류**: 528 bytes (잘못됨) vs 64 bytes (nvme-cli와 동일)
2. **버퍼 할당 순서 문제**: `start()`가 `set_nvme_ctrlr()` 전에 호출되어 DMA 버퍼 미할당

### Fix
- `stats_logger.h`: NvmeFdpStatsLog 구조체를 nvme-cli 형식과 동일하게 수정 (64 bytes)
  ```cpp
  struct NvmeFdpStatsLog {
      uint8_t hbmw[16];    // 128-bit
      uint8_t mbmw[16];    // 128-bit
      uint8_t mbe[16];     // 128-bit
      uint8_t rsvd48[16];  // Reserved
  };  // Total: 64 bytes
  ```
- `stats_logger.cpp`: `set_nvme_ctrlr()`에서 DMA 버퍼 할당 (호출 순서 무관하게 동작)
- `ftl_stats_logger.h/c`: 동일한 수정 적용

### Files Changed
- `module/bdev/icache/logcache/logging/stats_logger.h`
- `module/bdev/icache/logcache/logging/stats_logger.cpp`
- `lib/ftl/ftl_stats_logger.h`
- `lib/ftl/ftl_stats_logger.c`

---

## 2. Stream Classification Bug Fix (class_num garbage value)

### Problem
- segment의 `class_num`이 -1717986909 (0x99999983, garbage) 값으로 설정됨
- 성능 저하 및 대량의 에러 로그 발생

### Root Cause
`get_segment_with_stream_policy()`에서:
```cpp
uint64_t previous_blk_create_timestamp = UINT64_MAX;  // 문제!
if (exists(key)) {
    // ... 기존 key면 timestamp 가져옴
}
int stream_id = stream_policy->Classify(..., previous_blk_create_timestamp);
```

새로운 key (exists=false)일 때 `previous_blk_create_timestamp = UINT64_MAX`가 Classify에 전달됨.

`multi_hotcold_3` 정책에서:
```cpp
uint64_t time_diff = global_timestamp - created_timestamp;
// UINT64_MAX에서 빼면 underflow!
int gc_stream_id = time_diff / mTimestampGranularity;  // garbage!
```

### Fix
새로운 key일 때 `log_cache_timestamp`를 사용하도록 수정:
```cpp
uint64_t previous_blk_create_timestamp = log_cache_timestamp;  // Fix: 새 key는 현재 timestamp
if (exists(key)) {
    // ... 기존 key면 실제 timestamp 사용
}
```

이렇게 하면 `time_diff = 0`이 되어 stream 0에 할당됨.

### Files Changed
- `module/bdev/icache/logcache/port/log_cache.cpp`

---

## 3. Debug Logging Added

디버깅을 위해 다음 위치에 로그 추가:
- `alloc_segment()`: segment 할당 시 class_num 출력
- `get_segment_with_stream_policy()`: class_num 할당 전/후 값 출력
- `append_block_metadata()`: invalid class_num 검출

### Files Changed
- `module/bdev/icache/logcache/port/log_cache.cpp`
- `module/bdev/icache/logcache/port/log_cache_segment.h` (reset()에서 class_num=-1 초기화)
