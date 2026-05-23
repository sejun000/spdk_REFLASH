# GS_FINAL (PrGh) 정책 포팅 가이드 — segment-granular age_ghost

`delta_gc` 브랜치의 GS_FINAL 정책 중 **F_pred − F_ghost** 결정식 +
**segment-granular age_ghost_cache** 만 실 제품 코드에 옮길 때 필요한
변경점만 정리. 이전 `PORTING_GS_FINAL.md` 의 `LHS = r·waf·F_frac·D` 결정식을
대체.

## 1. 알고리즘 요약

매 host write tick:

(a) **매 (seg/4) tick** — EWMA 누적 갱신:
  - `G(u)`     = `compaction_ratio.update(host_ts, compacted_blocks)`
  - `G(u+δ)`   = `compaction_ratio_in_ghost_cache.update(host_ts, ghost_compacted_blocks_sum_)`
    (사전: `ghost_compacted_blocks_sum_ += s.cum_valid`)
  - `F(u)`     = `eviction_ratio.update(host_ts, evicted_blocks)`
  - `F_pred`   = `flush_pred_ratio.update(host_ts, ghost_flush_valid_sum_)`
    (사전: `ghost_flush_valid_sum_ += evictor->get_mth_score_valid_pages(D)`)
  - `F_ghost`  = `flush_ghost_ratio.update(host_ts, ghost_seg_valid_sum_)`
    (사전: `ghost_seg_valid_sum_ += age_ghost_cache.totalValidCount()`)

(b) **매 1 segment tick (decision)**:
  ```
  LHS = r · waf · (F_pred − F_ghost)
  RHS = G(u+δ)
  raise iff LHS > RHS
  target_valid_blk_rate = clamp(cur_util ± util_step, 0, hard_limit)
  ```

(c) **flush event** (`evict_segment()` 끝): 전체 flushed key vector 모아서
  `age_ghost_cache.pushSegment(keys)` + `++flush_event_count_`.

(d) **partial flush** (`evict_and_compaction()` 의 threshold-evict branch):
  flushed 된 key 만 모아서 `age_ghost_cache.pushSegment(keys)`.

(e) **host write hit** (`batch_insert`): `age_ghost_cache.invalidate(key)`.


## 2. 의존성

| 항목 | 설명 |
|---|---|
| CB heap | `evict_policy_cost_benefit::get_ghost_sum_for_free_segments(D)` — score 내림차순 D-segments-worth-of-free 의 cum_valid 반환. Gud 누적용. |
| Eviction heap top-D | `evict_policy::get_mth_score_valid_pages(D)` — 현재 evict heap 의 top-D segs valid pages 합. F_pred 누적용. |
| AgeGhostCache | 신규 segment-granular FIFO. §3. |
| 누적 carriers | `host_writes (log_cache_timestamp)`, `compacted_blocks`, `evicted_blocks`. |
| MovingAverageRatio | `updateFromCumulative(time, value)` 인터페이스. |


## 3. AgeGhostCache (segment-granular)

capacity 는 segments 단위 = D = `gs_decision_period_segs_`.

### 3.1 헤더 (`age_ghost_cache.h`)

```cpp
class AgeGhostCache {
public:
    explicit AgeGhostCache(std::size_t capacity_segs);

    void pushSegment(const std::vector<uint64_t>& blocks);
    bool invalidate(uint64_t block_id);

    uint64_t totalValidCount() const;        // Σ current valid_count over D segs

    void setCapacity(std::size_t capacity_segs);
    void reset();

private:
    struct GhostSeg {
        uint64_t              valid_count;
        std::vector<uint64_t> blocks;
    };
    using SegIt = std::list<GhostSeg>::iterator;
    std::size_t                         capacity_segs_;
    std::list<GhostSeg>                 segs_;
    std::unordered_map<uint64_t, SegIt> block_to_seg_;
    uint64_t                            total_valid_count_;
};
```

### 3.2 동작

- `pushSegment(blocks)`:
  1. 새 seg `{valid_count = blocks.size(), blocks}` 를 back 에 append.
  2. `total_valid_count_ += blocks.size()`.
  3. 각 block_id → seg iterator 매핑 (`block_to_seg_[b] = new_it`).
  4. `segs_.size() > capacity_segs_` 면 `evict_oldest()` 반복.

- `invalidate(b)`:
  1. `block_to_seg_` 에서 b 찾기. 없으면 false return.
  2. 해당 seg 의 `valid_count--`, `total_valid_count_--`.
  3. `block_to_seg_` 에서 b 제거.

- `evict_oldest()` (internal):
  1. `total_valid_count_ -= front.valid_count`.
  2. front 의 blocks 마다 `block_to_seg_` 에서 제거.
  3. `segs_.pop_front()`.

- `setCapacity(cap)`: `capacity_segs_ = cap`. `segs_.size() > cap` 동안
  `evict_oldest()`.

같은 block 이 두 번 push 되는 케이스는 불가능 (두번째 flush 되려면 log
cache 로 복귀 → host write → `invalidate()` 가 이미 매핑 제거). pushSegment
에 older-copy 처리 불필요.


## 4. LogCache 신규 멤버

```cpp
EwmaRatio     flush_pred_ratio;
double        ghost_flush_valid_sum_ = 0.0;

EwmaRatio     flush_ghost_ratio;
double        ghost_seg_valid_sum_ = 0.0;

AgeGhostCache age_ghost_cache;     // ctor 인자: gs_decision_period_segs_
```

`setMovingAverage(type, window_blocks)` 셋터 안에:

```cpp
flush_pred_ratio  = MovingAverageRatio::Make(type, window_blocks);
flush_ghost_ratio = MovingAverageRatio::Make(type, window_blocks);
```

`setPeriodicMode()` (또는 동등 init) 끝부분에:

```cpp
age_ghost_cache.setCapacity(static_cast<std::size_t>(gs_decision_period_segs_));
```


## 5. `periodic_ghost_delta_gc_sum_final()`

```cpp
void LogCache::periodic_ghost_delta_gc_sum_final() {
    if (!is_ghost_cache) return;

    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        update_ghost_compacted_blocks_sum_cum();  // §4.1 of PORTING_GS_FINAL
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(
            log_cache_timestamp, (uint64_t)ghost_compacted_blocks_sum_);
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);

        if (evictor) {
            const double target_segs = util_step_ * (double)total_segments;
            if (target_segs > 0.0) {
                const uint64_t top_n_valid =
                    evictor->get_mth_score_valid_pages(target_segs);
                ghost_flush_valid_sum_ += (double)top_n_valid;
            }
        }
        flush_pred_ratio.updateFromCumulative(
            log_cache_timestamp, (uint64_t)ghost_flush_valid_sum_);

        ghost_seg_valid_sum_ += (double)age_ghost_cache.totalValidCount();
        flush_ghost_ratio.updateFromCumulative(
            log_cache_timestamp, (uint64_t)ghost_seg_valid_sum_);
    }

    if (log_cache_timestamp % segment_size_blocks == 0) {
        if (!compaction_ratio.has_value() ||
            !compaction_ratio_in_ghost_cache.has_value() ||
            !eviction_ratio.has_value()) return;

        const double waf_w   = (current_waf > 0.0) ? current_waf : 1.0;
        const double Gud     = compaction_ratio_in_ghost_cache.value();
        const double F_pred  = flush_pred_ratio.has_value()
                             ? flush_pred_ratio.value()  : 0.0;
        const double F_ghost = flush_ghost_ratio.has_value()
                             ? flush_ghost_ratio.value() : 0.0;

        const double lhs = periodic_ratio_ * waf_w * (F_pred - F_ghost);
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


## 6. Hook 위치

| 위치 | 호출 |
|---|---|
| `batch_insert` host write loop, 기존 `ghost_cache.access(key)` 직후 | `age_ghost_cache.invalidate(key);` |
| `evict_segment(s)` valid block 처리 루프 내부 | `flushed_keys.push_back(blk.key);` |
| `evict_segment(s)` 함수 끝 | `if (is_ghost_cache && !flushed_keys.empty()) age_ghost_cache.pushSegment(flushed_keys);` + `++flush_event_count_;` |
| `evict_and_compaction(s, threshold, ...)` threshold-evict branch 의 valid block | `flushed_keys.push_back(blk.key);` |
| `evict_and_compaction` 함수 끝 (`reset_segment(s)` 직후) | `if (is_ghost_cache && !flushed_keys.empty()) age_ghost_cache.pushSegment(flushed_keys);` |
| Host write tick | `periodic_ghost_delta_gc_sum_final();` |
| Compact-vs-flush gate | 기존 그대로 (`if target_valid_blk_rate >= threshold compact else flush`) |

기존 `ghost_cache.push(blk.key)` (block-level legacy) 는 그대로 두고
`flushed_keys.push_back(blk.key)` 만 추가.


## 7. 포팅 체크리스트

1. `age_ghost_cache.h/cpp` 신규 추가 (§3). 빌드 시스템에 소스 등록.
2. LogCache 에 멤버 추가: `flush_pred_ratio`, `flush_ghost_ratio`,
   `ghost_flush_valid_sum_`, `ghost_seg_valid_sum_`, `age_ghost_cache`.
3. ctor 초기화 + `setMovingAverage` 셋터에 두 ratio `Make()` 한 줄씩.
4. `setPeriodicMode` 에 `age_ghost_cache.setCapacity(D)`.
5. `periodic_ghost_delta_gc_sum_final()` 의 (seg/4) tick 에 F_pred,
   F_ghost 누적 갱신 (§5).
6. Decision block: `LHS = r·waf·(F_pred − F_ghost), RHS = Gud`.
7. `batch_insert` host write 루프에 `age_ghost_cache.invalidate(key);`.
8. `evict_segment` / `evict_and_compaction` 에 `flushed_keys` vector
   누적 + 함수 끝에서 `pushSegment(flushed_keys)`.
9. `evictor->get_mth_score_valid_pages(D)` 인터페이스 노출.
10. `compactor->get_ghost_sum_for_free_segments(D)` 인터페이스 노출
    (GS_SUM 포팅 시 이미 추가됐으면 재사용).
