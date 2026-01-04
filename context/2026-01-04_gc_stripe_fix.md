# GC Stripe Boundary Fix & Segment Allocation Bug Fix

## Date: 2026-01-04

## 문제 1: GC 데이터 불일치 (50% 데이터 손상)

### 원인
- GC에서 16KB 배치 쓰기 시 stripe boundary를 넘는 경우 연속된 물리 주소가 아님
- `STRIPE_CHUNK_BLOCKS=32` (128KB), 4개 블록(16KB)이 stripe 경계를 넘으면 `get_block_offset()`이 비연속 주소 반환
- 연속 주소로 가정하고 scatter-gather write하면 잘못된 위치에 쓰여짐

### 수정 (log_cache_wrapper.cpp)
1. `GcIo` 구조체에 `expected_16k_writes` 필드 추가
2. GC write 전에 stripe boundary 체크하는 pre-calculation 루프 추가
3. Stripe 경계를 넘는 16KB 청크는 개별 4KB 블록으로 분리해서 쓰기
4. bounds checking 추가 (blocks 배열 접근 시)
5. GC entry point에 debug logging 추가

## 문제 2: GC Segfault (NULL pointer dereference)

### 증상
```
segfault at 0 ip ... in LogCache::prepare_gc at log_cache.cpp:1059
result.target_seg = (LogCacheSegment *) 0x0
```

### 원인
`alloc_segment()`에서 async mode일 때:
```cpp
// 수정 전: GC도 호스트 쓰기도 모두 블록됨
if (async_mode_ && free_pool.size() <= ASYNC_RESERVE_SEGMENTS) {
    return nullptr;
}
```
- `free_pool.size() <= 5`이면 GC도 segment 할당 실패
- GC가 target segment 없이 진행 → NULL dereference

### 수정 (log_cache.cpp)

1. **alloc_segment()**: GC는 예약 segment 사용 가능하도록 수정
```cpp
// shrink=true(호스트)만 블록, shrink=false(GC)는 항상 할당 가능
if (async_mode_ && shrink && free_pool.size() <= ASYNC_RESERVE_SEGMENTS) {
    return nullptr;
}
```

2. **prepare_gc()**: target_seg NULL 체크 추가 (방어적 프로그래밍)
```cpp
if (!result.target_seg) {
    result.do_evict_only = true;
    result.blocks_to_copy.clear();
    return true;
}
```

## 현재 상태
- fio는 끝까지 실행됨
- **verify fail 발생** - 내일 확인 필요
- 가능한 원인: `target_seg == nullptr`일 때 evict-only로 fallback하면서 valid 블록을 복사하지 않고 버림 → 데이터 손실

## 수정된 파일
- `module/bdev/icache/logcache/log_cache_wrapper.cpp`
- `module/bdev/icache/logcache/port/log_cache.cpp`

## 다음 할 일
- verify fail 원인 분석
- GC로 복사된 데이터가 올바른지 확인
- stripe boundary 넘는 경우 개별 쓰기가 제대로 동작하는지 확인
