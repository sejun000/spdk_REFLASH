# change.md — orig → Marginal REFLASH 결정 규칙 포팅 가이드

**대상**: orig(Fpred−Fghost) 정책이 들어가 있는 리얼 머신.
**목표**: `GhostDelta_GC_SUM_Final` (cache_policy `LOG_GREEDY_COST_BENEFIT_10_GS_FINAL`)의
GC-vs-flush 결정을 **marginal forward-difference 규칙**으로 교체.

> 결정식 변화
> - orig:     `LHS = r·waf·(F_pred − F_ghost)`, `RHS = Gud`, **do G if LHS > RHS**
> - marginal: `LHS = GG − 2·Gud`, `RHS = r·waf·(2F − FF)`, **do G if LHS < RHS**
>   - `Gud = ghost_sum(δN)`, `GG = ghost_sum(2δN)`, `F = get_mth(δN)`, `FF = get_mth(2δN)`
>   - `Gnext = GG−Gud`(2nd GC step), `Fnext = FF−F`(2nd flush step) 의 전개형.

---

## 0. 범위 — 바뀌는 파일은 둘뿐

```
log_cache.h    : +15 (멤버 3쌍 + setMovingAverage 3줄)
log_cache.cpp  : 결정 블록 교체 + 2-step lookahead 샘플링 블록 추가 + ctor 3줄
```
`evict_policy_cost_benefit.*`, `multi_hot_cold.*`, `icache.*` 등은 **건드리지 않음**.

## 1. 사전 조건 (orig에 이미 있어야 함 — 확인했음)

| 심볼 | 위치(orig) | 용도 |
|---|---|---|
| `CbEvictPolicy::get_mth_score_valid_pages(double m)` | evict_policy_cost_benefit.cpp:61 | F, FF |
| `CbEvictPolicy::get_ghost_sum_for_free_segments(double).cum_valid` | evict_policy_cost_benefit.cpp:97 | GG |
| `compactor` / `evictor` (unique_ptr<EvictPolicy>) | log_cache.h | 위 호출 대상 |
| `compaction_ratio_in_ghost_cache` (= Gud) | orig 이미 누적 중 | RHS의 Gud |

→ **새 메서드 추가 불필요.** orig이 이 helper들로 이미 Gud를 만들고 있으므로 그대로 재사용.

---

## 2. `log_cache.h` 변경 (2곳)

### [H1] 멤버 추가 — `flush_ghost_ratio` / `ghost_seg_valid_sum_` 선언 바로 뒤 (private:)

```cpp
    // 2-step lookahead candidate costs (GhostDelta_GC_SUM_Final).
    EwmaRatio gg_ratio;          // ghost_sum(2δN).cum_valid
    double    gg_ghost_sum_ = 0.0;
    EwmaRatio ff_ratio;          // get_mth(2δN)
    double    ff_flush_sum_ = 0.0;
    EwmaRatio gf_flush_ratio;    // get_mth(δN)  (GF's flush leg; GC leg = Gud)
    double    gf_flush_sum_ = 0.0;
```

### [H2] `setMovingAverage(...)` 안 — 기존 ratio Make() 호출들 끝에 3줄 추가

```cpp
        gg_ratio                        = MovingAverageRatio::Make(type, window_blocks);
        ff_ratio                        = MovingAverageRatio::Make(type, window_blocks);
        gf_flush_ratio                  = MovingAverageRatio::Make(type, window_blocks);
```
> setMovingAverage가 없는 빌드라면(=ctor에서만 초기화) [C1]만으로 충분.

---

## 3. `log_cache.cpp` 변경 (3곳)

### [C1] 생성자 initializer list — `flush_ghost_ratio(...)` 뒤 (콤마 주의)

```cpp
      flush_ghost_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      gg_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ff_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      gf_flush_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS))
```

### [C2] 2-step lookahead 샘플링 블록 추가
위치: `LogCache::periodic_ghost_delta_gc_sum_final()` 안, **`% (segment_size_blocks / 4)` 틱** 블록.
orig의 `flush_ghost_ratio.updateFromCumulative(...)` (F_ghost 갱신) **바로 뒤**에 삽입.

```cpp
        // 2-step lookahead candidates. δN = util_step_·N segments freed per step.
        //   GG: GC frees 2δN  → ghost_sum(2δN).cum_valid
        //   FF: flush frees 2δN → get_mth(2δN)
        //   F : flush frees δN  → get_mth(δN)   (GF's flush leg; GC leg = Gud)
        const double dN = util_step_ * static_cast<double>(total_segments);
        if (compactor) {
            gg_ghost_sum_ +=
                compactor->get_ghost_sum_for_free_segments(2.0 * dN).cum_valid;
            gg_ratio.updateFromCumulative(
                log_cache_timestamp, static_cast<uint64_t>(gg_ghost_sum_));
        }
        if (evictor) {
            ff_flush_sum_ +=
                static_cast<double>(evictor->get_mth_score_valid_pages(2.0 * dN));
            ff_ratio.updateFromCumulative(
                log_cache_timestamp, static_cast<uint64_t>(ff_flush_sum_));
            gf_flush_sum_ +=
                static_cast<double>(evictor->get_mth_score_valid_pages(dN));
            gf_flush_ratio.updateFromCumulative(
                log_cache_timestamp, static_cast<uint64_t>(gf_flush_sum_));
        }
```
> 독립 누적이라 orig의 F_pred/F_ghost 샘플링 블록은 **그대로 두면 됨**.

### [C3] 결정 블록 교체
위치: 같은 함수, **`% segment_size_blocks` 틱** 안의 `if (compaction_ratio.has_value() && ...)` 가드 내부.
`Gu/Fu/Gud/Fud/F_pred_rate/F_ghost_rate` 정의는 그대로 두고, **아래 3줄(orig)** 을 찾아서:

```cpp
            // ── orig (삭제) ──
            const double lhs = periodic_ratio_ * waf_w * (F_pred_rate - F_ghost_rate);
            const double rhs = Gud;
            const bool   raise = (lhs > rhs);
```

**이렇게 교체:**

```cpp
            // ── Marginal next-step comparison ──
            //   LHS = Gnext − G          = GG − 2·G
            //   RHS = r·waf·(F − Fnext)  = r·waf·(2F − FF)
            // do G (RAISE) if LHS < RHS, else flush (LOWER).
            (void)F_pred_rate;   // 결정에서 미사용 (logging만)
            (void)F_ghost_rate;  // 결정에서 미사용 (logging만)
            const double rwaf  = periodic_ratio_ * waf_w;
            const double GG    = gg_ratio.has_value()       ? gg_ratio.value()       : 0.0;
            const double FFv   = ff_ratio.has_value()       ? ff_ratio.value()       : 0.0; // get_mth(2δN)
            const double Fv    = gf_flush_ratio.has_value() ? gf_flush_ratio.value() : 0.0; // get_mth(δN)
            const double Gnext = GG  - Gud;
            const double Fnext = FFv - Fv;
            const double lhs   = Gnext - Gud;          // GG − 2·Gud
            const double rhs   = rwaf * (Fv - Fnext);  // r·waf·(2F − FF)
            const bool   raise = (lhs < rhs);
```
> 이후 `raw_target = raise ? cur_util+util_step_ : cur_util−util_step_` 액추에이터 로직은 orig과 동일 — 변경 없음.

---

## 4. 선택적 변경 (포팅 **안 해도** 정책 동작 동일)

- **[C4] F_pred 계산식 변경 (F_inv)** — `flush_pred_ratio`를 `ghost_flush_valid_sum_` 누적 대신
  `age_ghost_cache.totalPushValidCount() − totalPopValidCount()`로. **스킵 권장.** 이유:
  (1) marginal 결정은 F_pred를 안 씀(void)이라 `f_frac_pred` **로그 컬럼 값**만 바뀜.
  (2) `totalPush/PopValidCount()`는 `age_ghost_cache.h`(이 레포에서 untracked)에 있는 메서드라
  리얼 머신 orig의 age_ghost_cache에 없으면 **추가 구현이 필요**해짐. 굳이 할 이유 없음.
- **[C5] gsdec 로그 컬럼 추가** (`GGr FFr Fr Gnext Fnext`) — 디버깅/분석용. header 문자열과
  fprintf 포맷·인자만 늘리는 거라 동작 무관. 리얼 머신에서 안 찍어도 되면 생략.

---

## 5. 빌드 / 검증

1. **빌드**: `log_cache.h` 멤버를 추가했으므로 stale `.o` vtable 불일치(과거 SIGSEGV 전례) 방지 위해
   `make clean && make` 권장.
2. **dN 조절 주의**: `setPeriodicMode(GhostDelta_GC_SUM_Final)`가 ctor 뒤에
   `util_step_ = segment_size_blocks · gs_decision_period_segs_ / total_cache_block_count` 로 **덮어씀.**
   → **`--util_step`은 이 정책에서 무시되고 dN = `--gs_decision_period_segs`** 다. dN 스윕은 이 플래그로.
3. **동작 확인**: D=1 기준 정상상태에서 `Fv > Fnext` (≈+18~30%, mean Fv/Fnext≈1.2),
   `RHS = r·waf·(2F−FF) > 0` 대부분 양수면 정상. util은 target_valid_rate 중심으로 ±util_step 톱니.
4. **참고 수치(이 레포, alibaba_dwpd1to2_4x, scale2, 15.39TB host)**: TEC = host+comp+R·evict 기준
   D=1 r2.88 → 33.29(orig 32.25), D=1 r8.64 → 55.13(orig 55.13, 동률). marginal은 orig를 추월하진 않음.

---

## 부록: 원본 diff (참고용 — 손으로 적용 시 §2~§4가 source of truth)

> 아래는 `git diff HEAD -- log_cache.h log_cache.cpp` 전문. C4(F_pred), C5(logging)도 포함돼 있으니
> §4 권고대로 선별 적용할 것. 리얼 머신 orig이 동일 커밋이 아니면 `git apply`는 실패할 수 있음.

```diff
--- a/log_cache.cpp
+++ b/log_cache.cpp
@@ ctor initializer list @@
       flush_ghost_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS))
+      , gg_ratio(...), ff_ratio(...), gf_flush_ratio(...)   // [C1]

@@ periodic_ghost_delta_gc_sum_final(): % (segment_size_blocks/4) 틱 @@
-  // orig: ghost_flush_valid_sum_ += get_mth_score_valid_pages(target_segs);   ← [C4] 부분(스킵 권장)
-  flush_pred_ratio.updateFromCumulative(ts, ghost_flush_valid_sum_);
+  flush_pred_ratio.updateFromCumulative(ts, totalPushValidCount()-totalPopValidCount());  // [C4]
   ... flush_ghost_ratio.updateFromCumulative(...) ...
+  // [C2] 2-step lookahead: gg_ratio/ff_ratio/gf_flush_ratio 누적 (위 §3 [C2] 전체)

@@ periodic_ghost_delta_gc_sum_final(): % segment_size_blocks 틱, 결정 @@
-  const double lhs = periodic_ratio_ * waf_w * (F_pred_rate - F_ghost_rate);   // [C3] orig
-  const double rhs = Gud;
-  const bool   raise = (lhs > rhs);
+  // [C3] marginal: lhs = GG-2Gud, rhs = r·waf·(2F-FF), raise = (lhs < rhs)  (위 §3 [C3] 전체)

@@ gsdec 로그 header/fprintf @@
+  "... f_frac_pred GGr FFr Fr Gnext Fnext"   // [C5] optional
+  ..., F_pred_rate, GG, FFv, Fv, Gnext, Fnext);

--- a/log_cache.h
+++ b/log_cache.h
@@ private members (flush_ghost_ratio 뒤) @@
+  EwmaRatio gg_ratio; double gg_ghost_sum_ = 0.0;          // [H1]
+  EwmaRatio ff_ratio; double ff_flush_sum_ = 0.0;
+  EwmaRatio gf_flush_ratio; double gf_flush_sum_ = 0.0;
@@ setMovingAverage() @@
+  gg_ratio       = MovingAverageRatio::Make(type, window_blocks);   // [H2]
+  ff_ratio       = MovingAverageRatio::Make(type, window_blocks);
+  gf_flush_ratio = MovingAverageRatio::Make(type, window_blocks);
```
