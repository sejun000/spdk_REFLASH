# FDP TRIM Support and CDW12/CDW13 Passthrough

**Date:** 2026-01-21 ~ 2026-01-24
**Commits:** 2ff608a16 → 현재
**Branch:** base_v25_09

## Summary

FDP(Flexible Data Placement) 모드에서 TRIM 명령 지원 및 NVMe CDW12/CDW13 필드 패스스루 기능 추가.
FTL에서 User Data와 Metadata를 별도의 FDP placement handle로 분리하여 SSD WAF 최적화.

## Changes

### 1. lib/bdev/part.c
- `bdev_part_init_ext_io_opts()`에서 `nvme_cdw12`, `nvme_cdw13` 필드 전달 추가
- 파티션 bdev에서도 FDP directive 정보가 하위 디바이스로 전달됨

### 2. lib/nvme/nvme_ns_cmd.c
- `nvme_ns_cmd_rw_ext()`에서 `opts->cdw13` 사용하도록 변경 (기존: 0 고정)
- FDP write 로그 출력 코드 주석 처리 (성능 영향 방지)

### 3. module/bdev/icache/logcache/log_cache_config.h
새로운 FDP 설정 옵션 추가:
- `FDP_TRIM`: segment reset 시 TRIM/UNMAP 명령 전송 여부 (default: 1)
- `FDP_PLACEMENT_ENABLED`: placement handle 사용 여부 (default: 1)

### 4. module/bdev/icache/logcache/log_cache_wrapper.cpp
- `FdpTrimAsyncCtx` 구조체 추가 (비동기 TRIM 컨텍스트)
- `fdp_trim_completion()` 콜백 함수 구현
- `clear_zone_state_after_fdp_trim()` 함수 구현
- `reset_zone_async()`에서 FDP 모드일 때 TRIM 명령 전송 로직 추가
- TRIM 완료 후 zone state 클리어 처리
- 캐시 write/zone async 완료 시 pending I/O drain 트리거 추가

### 5. lib/ftl/ftl_core.c
- `ftl_invalidate_addr()`에서 addr=0 또는 FTL_ADDR_INVALID 스킵 처리 추가
- L2P 엔트리가 한번도 쓰이지 않았을 때 발생하는 addr=0 케이스 처리

### 6. lib/ftl/ftl_nv_cache.c
- Fire-and-forget TRIM → TRIM 완료 후 chunk를 free list에 추가하는 순차 처리로 변경
- TRIM 완료/실패 로깅 추가
- TRIM submission 실패 시 fallback 처리

### 7. lib/ftl/ftl_nv_cache_io.h
- `FTL_FDP_METADATA_ENABLED`를 0에서 1로 변경 (FDP 메타데이터 활성화)
- `ftl_nv_cache_bdev_write_blocks_with_md_fdp()`에 iov 파라미터 추가
- caller가 iov를 제공해야 함 (I/O 완료까지 유지 필요)

### 8. lib/ftl/mngt/ftl_mngt_bdev.c
- 밴드 크기 2GB → 1GB로 감소

### 9. lib/ftl/mngt/ftl_mngt_misc.c, ftl_mngt_startup.c, ftl_mngt_steps.h
- `ftl_mngt_trim_nv_cache()` 함수 추가
- 첫 시작(SPDK_FTL_MODE_CREATE) 또는 major upgrade 시 전체 NV 캐시 TRIM
- 시작 단계에 "TRIM NV cache" 스텝 추가

### 10. lib/ftl/nvc/ftl_nvc_bdev_non_vss.c, ftl_nvc_bdev_vss.c
- User data write 시 FDP handle 0 사용
- FTL_FDP_METADATA_ENABLED 조건부 컴파일

### 11. lib/ftl/utils/ftl_md.c, ftl_md.h
- `write_blocks()`에 iov, fdp_handle 파라미터 추가
- DATA_NVC(scrub)는 user data handle(0), 기타 메타데이터는 handle(1) 사용
- `struct ftl_md`와 `struct ftl_md_io_entry_ctx`에 iov 필드 추가

### 12. module/bdev/icache/logcache/port/log_cache.cpp, log_cache.h
- GC 트리거 로직 간소화: ASYNC_GC_TRIGGER_SEGMENTS 상수 제거, LOW_FREE_SEGMENTS 사용
- `LOW_FREE_SEGMENTS`: 25 → 15
- `get_segment_with_stream_policy()`: gc 모드에서만 기존 key의 timestamp 조회

### 13. module/bdev/icache/logcache/port/multi_hot_cold.cpp
- `mCheckCreatedTimestampOnly` 시 버그 수정: `global_timestamp` → `created_timestamp`

## Configuration

```c
// FDP TRIM 활성화/비활성화
#define FDP_TRIM 1  // 1: enable, 0: disable

// FDP Placement Handle 사용 여부
#define FDP_PLACEMENT_ENABLED 1  // 1: use actual handles, 0: force handle=0

// FDP Metadata 분리 (ftl_nv_cache_io.h)
#define FTL_FDP_METADATA_ENABLED 1  // 1: separate metadata to handle 1

// FDP Handles (ftl_nv_cache_io.h)
#define FTL_FDP_HANDLE_USER_DATA   0  // User data writes
#define FTL_FDP_HANDLE_METADATA    1  // Metadata writes (in-place overwrite)
```

## Notes

- FDP 모드에서 segment reset 시 TRIM을 보내면 SSD가 해당 영역을 invalidate하여 WAF 개선 가능
- User data(handle 0)와 Metadata(handle 1)를 분리하여 SSD 내부 GC 효율 향상
- TRIM은 이제 chunk free 전에 완료되어야 함 (fire-and-forget이 아님)
- NV 캐시 시작 시 TRIM으로 이전 데이터 정리
