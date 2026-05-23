# GhostDelta_GC_SUM_Final (GS_FINAL) 정책 포팅 가이드

본 문서는 `delta_gc` 브랜치의 **GS_FINAL** 정책만 실 제품 코드에 옮겨
심을 때 필요한 변경점만 추려서 정리. cum_valid 누적 + flush-event 평균
정책. r=8.64 D=1 기준 TEC 55.43 TB (GS 초기 reassign D=32 의 55.36 과
비슷한 수준이지만 GC 양은 6.45 → 약 36% 감소).


## 1. 알고리즘 한 줄 요약

매 host write tick (timestamp 증가) 마다:

(a) **매 (seg/4) tick**:
  - `ghost_compacted_blocks_sum_ += s.cum_valid` — compactor 의 CB-sorted
    상위 D segments (D = `util_step · N_seg`) 까지 누적된 valid pages.
  - EWMA 4 개 cumulative update:
    - `G(u)`  = `compaction_ratio.update(host_ts, compacted_blocks)`
    - `G(u+δ)` = `compaction_ratio_in_ghost_cache.update(host_ts, ghost_compacted_blocks_sum_)`
    - `F(u)`  = `eviction_ratio.update(host_ts, evicted_blocks)`
    - `F_frac` = `flush_avg_ratio.update(flush_event_count_ · seg_blocks, evicted_blocks)`
      (= avg valid fraction per real flush event)

(b) **매 1 segment tick (decision)**:
  ```
  LHS = r · waf · F_frac · util_step · total_segments     // = r·waf·F_frac·D
  RHS = G(u+δ)                                             // = Gud
  raise  if  LHS > RHS  else  lower
  target_valid_blk_rate ± util_step
  ```
  - `F_frac` 가 EWMA sample 없으면 **1.0** 로 fallback (보수적, RAISE 편향).
  - `RHS = Gud` 만 (Gu 빼지 않음): cum_valid 자체가 prediction-only marginal.

(c) **실제 flush event** (`evict_segment()` 끝): `flush_event_count_++`.

(d) **결정 결과**: target_valid_blk_rate 가 다음 tick 들의 compact-vs-flush
    gate (`if (target_valid_rate >= threshold) compact else flush`) 를 좌우.


## 2. 의존성 (실 제품에서 확보해야 함)

| 항목 | 설명 |
|---|---|
| **CB-sorted compaction victim heap** | `evict_policy_cost_benefit.h::get_ghost_sum_for_free_segments(D)` 의 가상 함수가 score 내림차순으로 D-segments-worth-of-free 까지 누적된 `cum_valid` / `cum_invalid` / `m` 를 반환. **free_sum 자체가 net free segments** (valid copy 비용 자동 차감, 자세히는 §6.2). |
| **누적 carriers** | `host_writes` (log_cache_timestamp), `compacted_blocks`, `evicted_blocks`. |
| **Moving average** | `updateFromCumulative(time, value)` 인터페이스. 두 cumulative 의 미분비를 EWMA sample 로 추출. |
| **Ghost cache** | (선택) `ghost_cache.evictCount()` 가 `F(u+δ)` 계산에 필요한데, FINAL 결정식에는 안 들어감. 호환만 유지하면 됨. |


## 3. 신규 상태 (LogCache 등가 객체)

`log_cache.h` 에 추가:

```cpp
// FINAL 전용 — cum_valid 가 prediction-only 단조 누적, dt × rate 없음
double   ghost_compacted_blocks_sum_ = 0.0;
uint64_t last_ghost_sum_ts_          = 0;
bool     ghost_sum_initialized_      = false;

// flush event 평균 — D-symmetric with GC cum_valid
uint64_t flush_event_count_          = 0;
MovingAverageRatio flush_avg_ratio;     // ctor: FromHalfLifeBlocks(...)

// 기존 4 ratio 와 함께 setMovingAverage(...) 에서 Make() 으로 교체.
```

`setMovingAverage(type, window_blocks)` 셋터 안에 한 줄 추가:

```cpp
flush_avg_ratio = MovingAverageRatio::Make(type, window_blocks);
```


## 4. 핵심 함수 두 개

### 4.1 `update_ghost_compacted_blocks_sum_cum()`

```cpp
void LogCache::update_ghost_compacted_blocks_sum_cum() {
    if (!compactor) return;
    const double target_free_segs = util_step_ * static_cast<double>(total_segments);
    auto s = compactor->get_ghost_sum_for_free_segments(target_free_segs);
    if (s.cum_invalid > 0.0) {
        ghost_compacted_blocks_sum_ += s.cum_valid;   // monotone accumulate
        ghost_sum_initialized_       = true;
        last_ghost_sum_ts_           = log_cache_timestamp;
    }
}
```

**중요 차이점 (이전 reassign 형과 다름)**:
- `dt × rate` 외삽 **없음**.
- `+= s.cum_valid` (단조 누적). 음수 가능성 없음.
- `target_free_segs = D` (= util_step·N_seg), inv_corr 같은 invalidate-rate
  보정 **없음** — `get_ghost_sum_for_free_segments` 의 free_sum 자체가 이미
  net free segments 라 보정 불필요 (§6.2 참고).


### 4.2 `periodic_ghost_delta_gc_sum_final()`

```cpp
void LogCache::periodic_ghost_delta_gc_sum_final() {
    if (!is_ghost_cache) return;

    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        update_ghost_compacted_blocks_sum_cum();
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(
            log_cache_timestamp,
            static_cast<uint64_t>(ghost_compacted_blocks_sum_));
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        flush_avg_ratio.updateFromCumulative(
            flush_event_count_ * segment_size_blocks, evicted_blocks);
    }

    if (log_cache_timestamp % segment_size_blocks == 0) {
        if (!compaction_ratio.has_value() ||
            !compaction_ratio_in_ghost_cache.has_value() ||
            !eviction_ratio.has_value()) return;

        const double waf_w  = (current_waf > 0.0) ? current_waf : 1.0;
        const double Gud    = compaction_ratio_in_ghost_cache.value();
        const double F_frac = flush_avg_ratio.has_value()
                            ? flush_avg_ratio.value() : 1.0;   // fallback 보수적

        const double lhs = periodic_ratio_ * waf_w * F_frac
                         * util_step_ * static_cast<double>(total_segments);
        const double rhs = Gud;
        const bool   raise = (lhs > rhs);

        const double cur_util = (total_cache_block_count > 0)
                              ? (double)global_valid_blocks / total_cache_block_count : 0.0;
        const double raw_target = raise ? (cur_util + util_step_) : (cur_util - util_step_);

        if (raise) target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, raw_target);
        else       target_valid_blk_rate = std::max(0.0, raw_target);
    }
}
```


## 5. Hook 위치

| 위치 | 호출 |
|---|---|
| Host write 처리 마지막 (write_size_to_cache 갱신 후, **매 host write tick**) | `periodic_ghost_delta_gc_sum_final();` |
| `evict_segment()` 함수 끝 (segment 1 개 flush 완료 직후) | `++flush_event_count_;` |
| Compaction victim 선정 직후 | (FINAL 모드에서는 **per-compact update 안 부름**. reassign sum 함수는 호출 금지.) |
| Compact-vs-flush gate | `if (target_valid_blk_rate >= util_step_) { compact } else { flush }` — 기존 로직 그대로. |

**가드 (기존 reassign 함수와 공존 시)**:
```cpp
if (is_ghost_cache && compactor) {
    update_ghost_compacted_blocks(victim);
    if (periodic_mode_ != PeriodicMode::GhostDelta_GC_SUM_Final) {
        update_ghost_compacted_blocks_sum();    // reassign — FINAL 에서 호출 금지
    }
}
```


## 6. 미묘한 점

### 6.1 F_frac 의 단위

`flush_avg_ratio.updateFromCumulative(flush_event_count_ × seg_blocks, evicted_blocks)` 의
sample `x = Δevicted_pages / (Δflush_events × seg_blocks)` = **avg valid
fraction per flush event** (0~1 range).

- 분모를 `seg_blocks` 로 곱하지 않으면 분자(page 단위) 와 분모(event 단위) 가
  dimensional mismatch. 위 형태가 unit-clean.
- EWMA value 자체가 fraction 이라 LHS 식에 추가 `/seg_blocks` 분모 불필요.

### 6.2 `get_ghost_sum_for_free_segments` 가 이미 net free

```cpp
free_sum += inv_frac;     // inv_i / seg_blocks
if (free_sum >= target_free_segments) break;
```

m segments GC 후의 실제 net free segments = `m − ⌈Σv_i/seg_blocks⌉
≈ Σinv_i/seg_blocks = free_sum`. 따라서 stop 조건 `free_sum >= D` 가
정확히 net free ≥ D segments. valid copy 가 새 segment 들 차지하는 비용이
자동 cancel out.

### 6.3 F_frac fallback 1.0 의 의미

cache fill 단계에서 flush 가 아직 안 일어나면 `flush_avg_ratio` sample 없음.
이때 `F_frac = 1.0` 으로 가정 = "100% valid segment evict" 의 보수적 가정
→ LHS 가 최대치 → RAISE 강제 → cache 가 차도록 유도. cache 가 차고 actual
flush 가 시작되면 EWMA 가 실제 값 (~0.3~0.5) 으로 수렴.

### 6.4 force-flush override

`check_and_evict_if_needed()` 의 `if (free_pool.size() <= 4 && compact) {
compact = false; }` 로직은 그대로 둠. target_valid_rate 가 RAISE 로 잡혀있어도
free 부족 시 강제 flush — 이때 발생하는 flush 도 `flush_event_count_++` 와
`evicted_blocks` 갱신에 자연스럽게 반영되어 F_frac EWMA 에 sample 로 들어감.


## 7. 검증된 측정값 (r=8.64, D=1, hl=1·seg)

| 항목 | 값 |
|---|---|
| host write | 15.391 TB |
| GC (compaction) | 6.452 TB |
| evict (flush to cold) | 3.888 TB |
| **TEC = host + GC + r·evict** | **55.43 TB** |
| BUtil (global_valid / total_cache) | 0.743 |
| GC victim count | 3,124 |

비교 (D=1 옛 식 LHS=r·waf·δ·N, hl=4·seg):
- GC 10.04 TB → 6.45 TB (**35% 감소**)
- evict 3.45 TB → 3.89 TB (**13% 증가**)
- TEC 55.24 → 55.43 (거의 동일, r·evict 가중치 영향 상쇄)


## 8. 포팅 체크리스트

1. PeriodicMode enum 에 `GhostDelta_GC_SUM_Final` 케이스 추가.
2. `flush_avg_ratio`, `flush_event_count_`, `ghost_compacted_blocks_sum_`
   상태 추가 + ctor 초기화.
3. `setMovingAverage(...)` 셋터에 `flush_avg_ratio = Make(...)` 한 줄 추가.
4. `update_ghost_compacted_blocks_sum_cum()` 신규 (§4.1).
5. `periodic_ghost_delta_gc_sum_final()` 신규 (§4.2).
6. Dispatcher (periodic tick 분기) 에 위 함수 호출 추가.
7. `evict_segment()` 끝에 `++flush_event_count_;`.
8. `compactor` 의 `get_ghost_sum_for_free_segments(D)` 인터페이스 노출
   (이미 GS_SUM 포팅 시 추가했으면 그대로 사용).
9. Cache type/policy 등록 (`createCache` 분기 + `setPeriodicMode`).

위 9 단계 외에 alibaba dwpd1to2 4x trace 같은 검증용 trace 한 번 돌려서
TEC ≈ 55 TB / BUtil ≈ 0.74 / GC ≈ 6.5 TB 범위 나오는지 sanity check.
