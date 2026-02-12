# 2026-02-12 Bugfix & Improvements

## 1. is_old_cycle_segment 버그 수정 (log_cache_wrapper.cpp)

**문제:** `is_old_cycle_segment()`에서 GC 세그먼트(`class_num >= GC_STREAM_START`)만 old cycle 체크를 했음. Host 세그먼트는 `idx < 0`이라 항상 `false` 반환 → old cycle 보호를 못 받음.

**수정:** `icache.cpp`와 동일하게, host 세그먼트도 `(seg->create_timestamp % g_cycle_length) / interval`로 stream idx를 추정해서 `g_stream_cycles[idx]` 비교.

```cpp
// Before: GC segment only
int idx = seg->class_num - Segment::GC_STREAM_START;
if (idx >= 0 && idx < IStream::MAX_STREAMS) { ... }

// After: All segments (GC + Host)
int idx = seg->class_num - Segment::GC_STREAM_START;
if (idx < 0 || idx >= IStream::MAX_STREAMS) {
    idx = static_cast<int>((seg->create_timestamp % g_cycle_length) / interval);
}
```

## 2. gc_dispatch_writes 64KB merge 체크 버그 수정 (log_cache_wrapper.cpp)

**문제:** 16개 블록을 64KB로 머지할 때, 첫 번째(`[0]`)와 마지막(`[15]`) 블록의 `dst_offset`과 `gc_placement_handle`만 비교. 중간 블록이 다른 stream/offset을 가져도 머지됨.

**수정:** 16개 블록 전체를 순회하면서 contiguous + same placement handle 검증.

```cpp
// Before: first/last only
do_merged = (last_off == first_off + 15 * block_size) && (first_ph == last_ph);

// After: all 16 blocks
do_merged = true;
for (size_t k = 1; k < BLOCKS_PER_64K; ++k) {
    if (w.dst_offset != first_off + k * block_size || w.gc_placement_handle != first_ph) {
        do_merged = false; break;
    }
}
```

**참고:** 다른 GC write 경로(non-seq, pipeline)는 이미 전체 순회하므로 정상.

## 3. run_benchmark.sh: replay 파일에 날짜시간 추가

replay 파일명에 타임스탬프 추가: `sepbit.replay` → `sepbit_20260212_183045.replay`

## 4. exit_benchmark.sh 신규 생성

`replay_trace`, `fio`, `run_*.sh`, `run_benchmark.sh` 프로세스를 `pkill -9`로 일괄 종료하는 스크립트.
