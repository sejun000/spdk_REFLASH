# GhostDelta_GC_SUM (GS) 정책 포팅 가이드

본 문서는 `delta_gc` 브랜치에서 개발된 **GhostDelta_GC_SUM (GS) v4 + EWMA**
정책을 실 제품 코드에 옮겨 심을 때 필요한 변경 사항을 정리한다.
- 결정 주기 `segs = 2` (segment 단위) **고정**
- Moving average: **EWMA** (half-life = 1 segment 분량 host writes)
- 최근 1~8 seg 실험에서 r=8.64 기준 segs=2 가 CSAL 대비 약 28% TEC 절감

검증된 시뮬레이션 결과 (r=8.64, 6 TiB prefill 이후 8 TiB 구간):

| Config | GC MB | r×Flush MB | TEC MB | vs CSAL |
|---|---|---|---|---|
| CSAL | 0 | 35,706,571 | 35,706,571 | 1.000 |
| **GS-EWMA segs=2** | 4,483,431 | 21,176,571 | **25,660,002** | **0.719** |


## 1. 알고리즘 한 줄 요약

매 host write tick:
- (a) 4 개의 cumulative ratio 를 EWMA 로 maintain:
  `G(u)` = `compacted/host_writes`,
  `G(u+δ)` = `ghost_compacted_sum/host_writes`,
  `F(u)` = `evicted/host_writes`,
  `F(u+δ)` = `evicted_in_ghost/host_writes`
- (b) 2 segment 마다 hill-climb 결정:
  ```
  if r·waf·(F(u)-F(u+δ)) > G(u+δ)-G(u):
      target_valid_rate += util_step    # raise → 더 compaction (flush 감소)
  else:
      target_valid_rate -= util_step    # lower → 더 flush
  ```
- (c) 실제 compaction 이벤트마다 `ghost_compacted_blocks_sum` 을
  `compacted_blocks + dt × (cum_valid/cum_invalid)` 로 anchor.
  `cum_*` 는 victim heap 을 CB-score 내림차순으로 스캔, 누적 free space 가
  `util_step × N_seg × inv_correction` 도달까지 모은 합산.


## 2. 의존성 (실 제품에서 확보해야 하는 것)

| 항목 | 설명 |
|---|---|
| **Ghost cache (eviction-side LRU)** | eviction 으로 빠진 block 의 LBA 를 일정 기간 유지하면서 access 가 들어오면 hit 카운트. `evictCount()` 가 핵심 — 그 hit 수가 "δ 만큼 더 보관했더라면 살아 있었을" 블록 수. |
| **CB-sorted compaction victim heap** | victim 선택 시 cost-benefit score 로 정렬되어 있는 자료구조. 본 정책은 거기서 **score 내림차순 순회** 가 필요. |
| **누적 carrier** | host write 누계, compacted block 누계, evicted block 누계, invalidate 누계. |
| **Moving average** | `updateFromCumulative(timestamp, cumulative_count)` 형태의 EWMA. 절대값 cumulative → 미분값 rate. |


## 3. 자료 구조 변경 (LogCache 등가 객체)

`segs=2` 고정 단순화 버전:

```cpp
// === GS_SUM 정책 상태 ===
// EWMA half-life. 1 segment 분량 host write (=segment_size_blocks).
double moving_avg_hl_blocks_   = static_cast<double>(segment_size_blocks);

// 4 개 ratio (모두 cumulative input -> per-host-write rate output)
MovingAverageRatio compaction_ratio_;              // G(u)
MovingAverageRatio compaction_ratio_in_ghost_;     // G(u+δ)
MovingAverageRatio eviction_ratio_;                // F(u)
MovingAverageRatio eviction_ratio_in_ghost_;       // F(u+δ)

// GhostDelta_GC_SUM 누적 anchored counter
double   ghost_compacted_blocks_sum_ = 0.0;
uint64_t last_ghost_sum_ts_          = 0;
bool     ghost_sum_initialized_      = false;
uint64_t last_invalidate_at_comp_    = 0;

// 정책 파라미터 (segs=2 고정)
static constexpr int    kGsDecisionPeriodSegs = 2;
double                  util_step_            = 0.02;   // δ = u_step
double                  target_valid_blk_rate = 0.0;    // 정책 출력값
double                  valid_blk_rate_hard_limit = 0.5;
double                  periodic_ratio_       = 8.64;   // r (CLI 옵션)
double                  current_waf           = 1.0;    // cold-tier WAF (없으면 1.0)
```

`util_step_` (= δ) 은 hill-climb 의 한 칸 폭이자 ghost-cache 크기 비율을
동시에 결정한다.  **GS_SUM 정책에서는 `δ` 의 분자가 segment 의 정수 배수**
가 되도록 자동 도출한다 — 결정 주기 (= segs segments) 의 host-write 분량과
한 칸 폭이 일치해야 hill-climb 의 1 step 이 ghost cache 가 표현하는 영역과
동일해진다.

```cpp
// 정책 진입 시 (setPeriodicMode 등가 위치) 자동 도출
util_step_ = static_cast<double>(segment_size_blocks * kGsDecisionPeriodSegs)
           / static_cast<double>(total_cache_block_count);
// 예 1: segment=6 GiB,  cache=1.71 TiB, segs=2 → δ = 12 GiB / 1.71 TiB ≈ 0.0069
// 예 2: segment=64 MiB, cache=2 GiB,    segs=2 → δ = 128 MiB / 2 GiB   = 0.0625

// ghost cache capacity = total_cache_block_count × util_step_ (=  segs × segment_size_blocks)
ghost_cache_.set_capacity(static_cast<std::size_t>(
    static_cast<double>(total_cache_block_count) * util_step_));
```

> **CLI override**: `cache_sim` 은 `--util_step 0.02` 로 자동 도출값을 덮어쓰는
> 옵션을 제공한다 (실험 환경에서 cache 가 매우 커서 자동값이 작아질 때 적용).
> 본 시뮬레이션 결과들 (CSAL 대비 0.719) 은 **자동 도출값이 아니라 CLI override
> 0.02** 로 돈 것이므로, 실 제품 포팅 시에도 자동 도출만으로는 동일 거동이
> 안 나올 수 있다. 다음 둘 중 하나를 권장:
>
> 1. **이론에 충실**: 자동 도출값 그대로 사용 (`δ = segs · seg / cache`). ghost
>    cache 가 작아지지만 정의에 부합.
> 2. **실험과 동일**: `util_step` 을 외부 파라미터로 노출하고 0.02 같은 값을
>    명시적으로 주입. ghost cache 도 그에 맞춰 그만큼.


## 4. MovingAverage (EWMA) 클래스 — 신규

기존 production 코드에 EWMA-from-cumulative 가 없으면 추가해야 한다.

```cpp
// emwa.h --- 약식 (cumulative 기반 EWMA 추출)
class Ewma {
public:
    Ewma(double half_life_in_blocks)
        : half_life_(half_life_in_blocks) {}

    // ts: host_write 누계 (절대 단조 증가)
    // cum: 어떤 사건 누계 (compacted blocks 등)
    void updateFromCumulative(uint64_t ts, uint64_t cum) {
        if (!has_prev_) {
            prev_ts_  = ts;
            prev_cum_ = cum;
            has_prev_ = true;
            return;
        }
        const uint64_t dt = (ts > prev_ts_) ? (ts - prev_ts_) : 0;
        if (dt == 0) return;

        const double rate     = double(cum - prev_cum_) / double(dt);
        // EWMA decay over dt host writes (half-life in same unit).
        const double alpha    = 1.0 - std::pow(0.5, double(dt) / half_life_);
        value_       = has_value_ ? value_ + alpha * (rate - value_) : rate;
        has_value_   = true;

        prev_ts_  = ts;
        prev_cum_ = cum;
    }

    bool   has_value() const { return has_value_; }
    double value()     const { return value_; }
    void   reset() { has_prev_ = has_value_ = false; value_ = 0.0; }

private:
    double  half_life_;
    bool    has_prev_  = false;
    bool    has_value_ = false;
    uint64_t prev_ts_  = 0;
    uint64_t prev_cum_ = 0;
    double  value_     = 0.0;
};

// emwa_ratio.h --- 그냥 thin wrapper. 정책 전환 안 한다면 unique_ptr 없이 직접 Ewma 사용해도 됨.
using MovingAverageRatio = Ewma;
```

half-life = 1 segment 분량 host writes (= `segment_size_blocks` 절대값).


## 5. CB victim heap 의 누적 스캔 API — 신규

기존 evict policy 가 best-score 1 개만 노출한다면, 새 API 1 개 추가 필요.

```cpp
// evict_policy.h
struct GhostSumResult {
    double cum_valid   = 0.0;   // 누적 valid pages
    double cum_invalid = 0.0;   // 누적 invalid pages
    double m           = 0.0;   // 누적 segment 수 (소수 가능)
};

// score 내림차순으로 victim heap 을 훑으며 누적 free-space 가
// target_free_segments(=segment 단위) 에 도달할 때까지 더한다.
GhostSumResult get_ghost_sum_for_free_segments(double target_free_segments) const;
```

구현:

```cpp
// evict_policy_cost_benefit.cpp
GhostSumResult CbEvictPolicy::get_ghost_sum_for_free_segments(
        double target_free_segments) const
{
    GhostSumResult r;
    if (target_free_segments <= 0.0 || heap_.empty()) return r;

    double free_sum = 0.0;
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        const uint64_t v   = it->seg->valid_cnt;
        const uint64_t inv = (pages_in_segment > v) ? (pages_in_segment - v) : 0;
        const double inv_frac =
            static_cast<double>(inv) / static_cast<double>(pages_in_segment);

        if (free_sum + inv_frac >= target_free_segments) {
            // 경계 segment 는 분수만큼 비례 채움
            const double need = target_free_segments - free_sum;
            const double frac = (inv > 0)
                ? need * static_cast<double>(pages_in_segment) / static_cast<double>(inv)
                : 0.0;
            r.cum_valid   += static_cast<double>(v)   * frac;
            r.cum_invalid += static_cast<double>(inv) * frac;
            r.m           += frac;
            break;
        }
        free_sum      += inv_frac;
        r.cum_valid   += static_cast<double>(v);
        r.cum_invalid += static_cast<double>(inv);
        r.m           += 1.0;
    }
    return r;
}
```


## 6. 핵심 함수 — segs=2 고정 단순화 버전

### 6.1  실제 compaction event hook

`update_ghost_compacted_blocks_sum()` — **트리거 = 실 compaction event**.
host-write tick 단위가 아니라 **compaction 이 실제로 발생하는 시점마다 한 번**
호출한다.  따라서 호출 빈도는 정책 활동에 따라 가변적이다: cache 가 잘 채워져
compaction 이 자주 일어나면 dt 가 작고, 한산하면 dt 가 크다.

- 호출 위치: `compactor` 가 victim segment 를 선정한 직후, 그 victim 의
  valid 페이지를 새 segment 로 복사하기 직전.
- `dt = log_cache_timestamp - last_ghost_sum_ts_` 는 "직전 comp event 이후
  누적된 host write 양". `ghost_compacted_blocks_sum_` 의 증가량은 그 구간
  동안 θ-regime CB-tail 이 *만약* 동일 호스트 쓰기 분량을 GC 했더라면
  발생했을 valid copy 수.
- 첫 호출에서는 `dt = 0` 이라 ghost 증가량 없이 `compacted_blocks` 에만
  anchor (init).

```cpp
void LogCache::update_ghost_compacted_blocks_sum() {
    if (!compactor_) return;

    // 1) invalidation rate 보정. host write 가 freshly freed 영역을 침범하면
    //    실제 net free-space 회수가 깎이므로 target_free_segs 를 늘려준다.
    double inv_corr = 1.0;
    if (ghost_sum_initialized_ && log_cache_timestamp > last_ghost_sum_ts_) {
        const uint64_t win_writes = log_cache_timestamp - last_ghost_sum_ts_;
        const uint64_t win_inv    = (invalidate_blocks > last_invalidate_at_comp_)
                                  ? (invalidate_blocks - last_invalidate_at_comp_)
                                  : 0;
        const double   i_rate  = std::min(0.95,
            static_cast<double>(win_inv) / static_cast<double>(win_writes));
        const double   theta_i = util_step_ * i_rate;
        inv_corr = 1.0 / (1.0 - std::min(0.95, theta_i));
    }

    // 2) target_free_segments = util_step × N_seg × inv_corr
    const double target_free_segs =
        util_step_ * static_cast<double>(total_segments_) * inv_corr;
    const auto s = compactor_->get_ghost_sum_for_free_segments(target_free_segs);
    if (s.cum_invalid <= 0.0) return;

    // 3) anchored advance:
    //    ghost_sum = compacted_blocks + dt × (cum_valid / cum_invalid)
    //    → G(u+δ) − G(u) ≈ Σ_window(dt × rate) / Σ_window(dt) = rate (single window).
    if (ghost_sum_initialized_) {
        const double rate = s.cum_valid / s.cum_invalid;
        const uint64_t dt = (log_cache_timestamp > last_ghost_sum_ts_)
                          ? (log_cache_timestamp - last_ghost_sum_ts_) : 0;
        ghost_compacted_blocks_sum_ = static_cast<double>(compacted_blocks)
                                    + static_cast<double>(dt) * rate;
    } else {
        // 첫 comp: anchor 만 잡고 끝 (dt=0)
        ghost_compacted_blocks_sum_ = static_cast<double>(compacted_blocks);
        ghost_sum_initialized_      = true;
    }
    last_ghost_sum_ts_       = log_cache_timestamp;
    last_invalidate_at_comp_ = invalidate_blocks;
}
```

### 6.2  Host-write tick periodic

`periodic_gs_sum()` — **매 host write 마다** 호출 (또는 segment_size_blocks/4 마다
조건문 검사). `log_cache_timestamp` 는 host write 누계.

```cpp
void LogCache::periodic_gs_sum() {
    // 6.2.1  매 segment/4 마다 4 개 EWMA ratio 업데이트
    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        compaction_ratio_.updateFromCumulative(
            log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_.updateFromCumulative(
            log_cache_timestamp, static_cast<uint64_t>(ghost_compacted_blocks_sum_));
        eviction_ratio_.updateFromCumulative(
            log_cache_timestamp, evicted_blocks);
        eviction_ratio_in_ghost_.updateFromCumulative(
            log_cache_timestamp, ghost_cache_.evictCount());
    }

    // 6.2.2  매 2 segment 마다 hill-climb 결정
    if (log_cache_timestamp %
        (segment_size_blocks * kGsDecisionPeriodSegs) != 0) return;

    if (!(compaction_ratio_.has_value()
       && compaction_ratio_in_ghost_.has_value()
       && eviction_ratio_.has_value()
       && eviction_ratio_in_ghost_.has_value())) return;

    const double G_u   = compaction_ratio_.value();
    const double G_ud  = compaction_ratio_in_ghost_.value();
    const double F_u   = eviction_ratio_.value();
    const double F_ud  = eviction_ratio_in_ghost_.value();
    const double waf_w = (current_waf > 0.0) ? current_waf : 1.0;

    // Flush 절감 vs Comp 증가 비교
    //   benefit of staying longer in cache  : r·waf·(F(u) − F(u+δ))
    //   cost     of staying longer in cache : G(u+δ) − G(u)
    if (periodic_ratio_ * waf_w * (F_u - F_ud) > (G_ud - G_u)) {
        // raise target → cache 더 가득 채워 flush 줄임
        target_valid_blk_rate = std::min(valid_blk_rate_hard_limit,
            static_cast<double>(global_valid_blocks) / total_cache_block_count
            + util_step_);
    } else {
        // lower target → flush 더 함
        target_valid_blk_rate = std::max(0.0,
            static_cast<double>(global_valid_blocks) / total_cache_block_count
            - util_step_);
    }
}
```


## 7. Hook 위치 (실 제품 코드 측 변경)

| 위치 | 호출 |
|---|---|
| Host write 처리 마지막 (write_size_to_cache 갱신 후, **매 host write tick**) | `periodic_gs_sum();` |
| Compaction victim 선정 직후, valid 복사 직전 (**매 compaction event**) | `update_ghost_compacted_blocks_sum();` |
| Evict (cache→cold flush) 시 ghost cache 에 LBA 등록 | `ghost_cache_.insert(lba);` |
| Host read/write 시 ghost cache lookup → hit 시 `evictCount++` | `ghost_cache_.access(lba);` |
| `check_and_evict_if_needed` 의 compact-vs-evict gate | `if (target_valid_blk_rate >= util_step_) { compact path } else { evict path }` |

`current_waf` 는 cold tier device 의 NAND WAF 인데, 측정 인프라가 없으면 1.0 고정해도 정책은 동작. (단 cold-tier GC 가 활발한 환경에선 가중치가 의미 있어짐.)


## 8. 초기화 / CLI 매핑

| CLI / config | 코드 | 기본값 |
|---|---|---|
| `--periodic_ratio 8.64` | `periodic_ratio_` | 8.64 |
| `--util_step 0.02` | `util_step_` (자동 도출값을 override) | 0.02 (실험 사용값) |
| (고정) | `kGsDecisionPeriodSegs` | 2 |
| (고정) | EWMA half-life | `segment_size_blocks` |
| `--cold_capacity ...` | cold-tier 용량 | 환경별 |

`util_step` 의 결정 순서:
1. 정책 진입 시 자동 도출 → `(segs × segment_size_blocks) / total_cache_block_count`
2. CLI/config 에 명시값이 있으면 그것으로 override (ghost_cache 크기는 그 시점
   `total_cache_block_count × util_step_` 으로 재계산)

ghost cache 크기 = `total_cache_block_count × util_step_` (정책상 정의에 의해
`segs × segment_size_blocks` 와 같음, 단 override 시엔 별도).


## 9. (선택) Stream interval 동적화 — `istream.cpp` / `multi_hot_cold.cpp`

Multi-hot-cold stream 분류를 쓰는 경우, GC stream 의 timestamp granularity 가
GS 정책의 `g_threshold` (= "이 시간보다 오래된 block 은 old" 기준점) 와
동기화돼야 stream classification 이 의미 있다.

### 9.0  `g_threshold` 가 뭔지

전역 변수. `log_cache.cpp` 에서 host write 누계의 "방금 cold tier 로 flush 된
block 의 create_timestamp" 를 기록한다 — 즉 *현재 cache 가 보관 중인 가장
오래된 데이터의 age 경계*.

- **초기화** (cache ctor): `g_threshold = cache_block_count * 2` —
  pre-warmup 동안 어떤 block 도 "old" 로 보이지 않도록 안전한 큰 값.
- **갱신** (compaction tick / evict path): victim 으로 선택된 segment 의
  oldest valid block 의 create_timestamp 로 업데이트 (+ segment 크기만큼
  보정). cache 가 채워질수록 자연스럽게 작아진다 (age 경계가 좁아짐).
- **소비자**: stream classifier 의 timestamp granularity (이 절 9.1).
  Live-vs-old 분류 기준.

기존 코드는 `set_stream_interval(cache_block_count, segment_size_blocks)` 한
번만 호출하고 interval = `cache_block_count / 3` 로 고정했다. 그러면
g_threshold 가 운영 중에 줄어들어도 GC stream 의 granularity 가 안 따라가
older block 분류가 어긋난다. 이 절의 변경은 **매 Classify() 호출마다
fresh interval 을 g_threshold 로부터 재계산** 하는 것.

### 9.1  `compute_stream_interval` 신규 함수

```cpp
// istream.cpp 의 신규 함수
uint64_t compute_stream_interval(uint64_t fallback_cache_blocks /*=0*/) {
    uint64_t base = (g_threshold > 0)
                  ? g_threshold
                  : (fallback_cache_blocks > 0
                      ? fallback_cache_blocks
                      : g_stream_fallback_blocks);
    if (base == 0) return interval;            // keep prior

    uint64_t computed = base / kMultiHotColdStreams;  // 5 streams 기본
    if (computed == 0) computed = 1;
    // segment 정렬 (set_stream_interval 에서 저장한 segment_size_blocks 사용)
    if (g_stream_segment_size_blocks > 0) {
        uint64_t seg = g_stream_segment_size_blocks;
        computed = ((computed + seg - 1) / seg) * seg;
        if (computed == 0) computed = seg;
    }
    return computed;
}

// multi_hot_cold.cpp Classify() 진입 시 매번 refresh
int MultiHotCold::Classify(...) {
    uint64_t fresh = compute_stream_interval(0);
    if (fresh > 0 && static_cast<uint64_t>(mTimestampGranularity) != fresh) {
        mTimestampGranularity = static_cast<int>(fresh);
        g_cycle_length        = static_cast<uint64_t>(mTimestampGranularity) * mMaxGcStreams;
    }
    // ... 이하 원래 분류 로직
}
```

### 9.2  추가 변경 사항 요약

| 위치 | 변경 |
|---|---|
| `istream.h` | `uint64_t compute_stream_interval(uint64_t fallback_cache_blocks=0);` 선언 |
| `istream.cpp` 익명 namespace | `g_stream_segment_size_blocks`, `g_stream_fallback_blocks` 추가 (set_stream_interval 에서 채움) |
| `set_stream_interval()` 구현 | 기존: `interval = cache_block_count / 3`. 신규: 두 글로벌 저장 후 `interval = compute_stream_interval(cache_block_count)` |
| `multi_hot_cold.cpp Classify()` | 진입부에서 fresh refresh (위 9.1 코드) |
| `g_threshold` 갱신 | 기존 LogCache 의 compaction 코드에 이미 존재 — 추가 변경 없음 |

`kMultiHotColdStreams` 는 기본 5 (config 따라 변경). stream classifier 안 쓴다면 이 절 전체 무시 가능.


## 10. 검증 체크리스트

1. **초기 상태**: 정책 시작 시점 `target_valid_blk_rate = 0.0` 이어야 무한 flush
   상태에서 시작 (warm-up 시 cache 채워지면서 hill-climb 가 끌어올림).
2. **EWMA `has_value()` gate**: 4 개 ratio 가 모두 valid 되기 전엔 hill-climb
   skip — 초기 노이즈로 잘못 raise/lower 되지 않음.
3. **CB-sorted scan 순서**: `ordered_begin()` 이 score 내림차순임을 확인.
   victim 1 개 pop 과 같은 score function 이어야 함.
4. **`compacted_blocks` baseline**: `ghost_sum_initialized_` 첫 trigger 에서
   `compacted_blocks` 가 이미 양수일 수 있음 (warm-up). 그 자체로 OK — `dt=0`
   이라 increment 0.
5. **Stale .o 주의**: 헤더에 멤버 추가 후 dependent .o 가 빌드되지 않으면
   vtable 불일치로 SIGSEGV 가능. 첫 빌드는 `make clean && make` 권장.


## 11. 파일별 diff (브랜치 `delta_gc` 기준 main 대비, GS 핵심 부분만)

전체 patch 는 같은 디렉토리의 `delta_gc_full.patch` 참고. 아래는 포팅에 직접
필요한 hunk 만.

### 11.1  `evict_policy.h` (신규 API 선언)

```diff
+    struct GhostSumResult {
+        double cum_valid   = 0.0;
+        double cum_invalid = 0.0;
+        double m           = 0.0;
+    };
+    virtual GhostSumResult get_ghost_sum_for_free_segments(double target_free_segments) const {
+        return {};
+    }
```

### 11.2  `evict_policy_cost_benefit.{h,cpp}` (구현)

```diff
+    GhostSumResult get_ghost_sum_for_free_segments(double target_free_segments) const override;
```

```diff
+EvictPolicy::GhostSumResult
+CbEvictPolicy::get_ghost_sum_for_free_segments(double target_free_segments) const
+{
+    GhostSumResult r;
+    if (target_free_segments <= 0.0 || heap_.empty()) return r;
+    double free_sum = 0.0;
+    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
+        const uint64_t v = it->seg->valid_cnt;
+        const uint64_t inv = (pages_in_segment > v) ? (pages_in_segment - v) : 0;
+        const double inv_frac = double(inv) / double(pages_in_segment);
+        if (free_sum + inv_frac >= target_free_segments) {
+            const double need = target_free_segments - free_sum;
+            const double frac = (inv > 0) ? need * double(pages_in_segment) / double(inv) : 0.0;
+            r.cum_valid   += double(v)   * frac;
+            r.cum_invalid += double(inv) * frac;
+            r.m           += frac;
+            break;
+        }
+        free_sum      += inv_frac;
+        r.cum_valid   += double(v);
+        r.cum_invalid += double(inv);
+        r.m           += 1.0;
+    }
+    return r;
+}
```

### 11.3  `log_cache.h` (멤버 추가)

```diff
+    double   ghost_compacted_blocks_sum_ = 0.0;
+    uint64_t last_ghost_sum_ts_          = 0;
+    bool     ghost_sum_initialized_      = false;
+    uint64_t last_invalidate_at_comp_    = 0;
+    double   util_step_                  = 0.02;
+    MovingAverageRatio compaction_ratio_;
+    MovingAverageRatio compaction_ratio_in_ghost_cache_;
+    MovingAverageRatio eviction_ratio_;
+    MovingAverageRatio eviction_ratio_in_ghost_cache_;
+    void update_ghost_compacted_blocks_sum();
+    void periodic_gs_sum();
```

### 11.4  `log_cache.cpp` (위 6 번 함수 본문 그대로 + 호출 위치)

Compaction path 안:
```diff
+    update_ghost_compacted_blocks_sum();
```

Host-write tick (`periodic()` 또는 매 write 호출자):
```diff
+    periodic_gs_sum();
```


## 12. 운영상 메모

- segs=1 보다 segs=2 가 좋은 이유: 결정 주기가 너무 짧으면 EWMA 가 한 segment
  내 노이즈에 흔들려 raise/lower 가 진동. 4 이상으로 늘리면 반응이 늦어져
  flush 가 많아짐.
