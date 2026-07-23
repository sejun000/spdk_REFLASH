# porting_last — marginal-cost → invrate rule (현재 구현 diff)

**baseline (d1-inv 직전)** = `HEAD 4771444` "Add marginal forward-diff decision rule to GS_SUM_Final".
**target** = 현재 작업트리 (invrate rule, anti-stuck cap 없음).
정책: `LOG_GREEDY_COST_BENEFIT_10_GS_FINAL` → `LogCache::periodic_ghost_delta_gc_sum_final()`.

> 산문 가이드/파일별 설명은 `PORTING_GS_FINAL_INVRATE.md`. 이 문서는 **그대로 적용 가능한 diff**.

---

## 핵심: decision rule 교체

**BEFORE (marginal forward-diff, HEAD):**
```cpp
const double Gnext = GG  - Gud;
const double Fnext = FFv - Fv;
const double lhs   = Gnext - Gud;          // GG − 2·Gud
const double rhs   = rwaf * (Fv - Fnext);  // r·waf·(2F − FF)
const bool   raise = (lhs < rhs);
```

**AFTER (invrate, 현재):**
```cpp
// victim v = util_step_·N free 만들 때 건드리는 victim 중 max WT
const double vic_target_free = util_step_ * (double)total_segments;
EvictPolicy::VictimWtSpanResult vspan;
double invrate_sum = 0.0;
if (compactor) {
    vspan = compactor->get_victim_wt_span_for_free_segments(vic_target_free);
    invrate_sum = sum_invalidate_rate_in_wt_range(vspan.max_wt);  // WT ≤ v 전부
}
const double lhs   = Gud;                            // GC 복사비용 / host page
const double rhs   = invrate_sum * periodic_ratio_;  // invrate·r / host page
bool         raise = (lhs < rhs);
// anti-stuck cap(15 RAISE→1 LOWER) 없음 — 결정은 위 비교가 전부.
```

지원 머신(diff에 포함): per-seg invalidation rate(segment.h `invalidate_count_`/`note_invalidation()`/
`fold_invalidate_rate()`/`invalidate_rate()`), periodic fold loop, `get_victim_wt_span_for_free_segments`
+`for_each_victim_in_order`(evict_policy), `sum_invalidate_rate_in_wt_range`(log_cache), `Ewma::update` 헤더 inline.
*(istream/multi_hot_cold `ClassifyReadOnly` + `simulate_gc_net_free` + `gcsim_*` 컬럼은 GS_DECISION_LOG 진단용 — rule 무관.)*

## 빌드 (CLAUDE.md §8)
`segment.h`/`emwa.h`/`evict_policy.h` ABI 변경 → **`make clean && make cache_sim`** (stale .o SIGSEGV 방지).

## TEC 결과 (alibaba_dwpd1to2_4x, gsD1, scale2, hl=1seg; cap 포함 바이너리)
| r | invrate | lambda D1 | Δ | orig Fpred |
|---|---|---|---|---|
| 2.88 | **32.35** | 33.45 | −1.11 (−3.3%) | 32.25 |
| 8.64 | **54.60** | 55.74 | −1.13 (−2.0%) | 55.13 |

---

## 전체 diff (`git diff HEAD` — *.cpp *.h)
```diff
diff --git a/emwa.cpp b/emwa.cpp
index cb8e504..1626fe9 100644
--- a/emwa.cpp
+++ b/emwa.cpp
@@ -41,11 +41,7 @@ void Ewma::reset() {
     bias_prod_ = 1.0;
 }
 
-void Ewma::update(double x) {
-    updateWithAlpha(x, alpha_);
-    steps_ += 1;
-    bias_prod_ *= (1.0 - alpha_);
-}
+// Ewma::update — now inline in emwa.h (hot per-event path).
 
 void Ewma::updateWithUnits(double x, double units) {
     if (units <= 0.0) throw std::invalid_argument("units must be > 0");
@@ -72,11 +68,4 @@ double Ewma::clampAlpha(double a) {
     return a;
 }
 
-void Ewma::updateWithAlpha(double x, double alpha_eff) {
-    if (!initialized_) {
-        m_ = x;
-        initialized_ = true;
-    } else {
-        m_ = alpha_eff * x + (1.0 - alpha_eff) * m_;
-    }
-}
+// Ewma::updateWithAlpha — now inline in emwa.h.
diff --git a/emwa.h b/emwa.h
index 646d10b..1027596 100644
--- a/emwa.h
+++ b/emwa.h
@@ -35,8 +35,13 @@ public:
 
     void reset() override;
 
-    // 한 단위(step) 업데이트
-    void update(double x);
+    // 한 단위(step) 업데이트. 핫 경로(세그먼트별 invalidation 폴딩 등)에서 호출
+    // 오버헤드를 없애려 헤더 inline 정의. exp/log 없는 고정-α 재귀.
+    void update(double x) {
+        updateWithAlpha(x, alpha_);
+        steps_ += 1;
+        bias_prod_ *= (1.0 - alpha_);
+    }
 
     // 임의의 경과 "단위 수"로 가중 업데이트 (가중치=units)
     void updateWithUnits(double x, double units);
@@ -60,7 +65,12 @@ public:
     std::uint64_t steps() const { return steps_; }
 private:
     static double clampAlpha(double a);
-    void updateWithAlpha(double x, double alpha_eff);
+    // inline (header) so update() above and updateWithUnits() in the .cpp both
+    // inline the core recurrence; no exp/log here.
+    void updateWithAlpha(double x, double alpha_eff) {
+        if (!initialized_) { m_ = x; initialized_ = true; }
+        else { m_ = alpha_eff * x + (1.0 - alpha_eff) * m_; }
+    }
 
     double        alpha_;
     bool          bias_correction_;
diff --git a/evict_policy.h b/evict_policy.h
index 6a9c03f..37c8180 100644
--- a/evict_policy.h
+++ b/evict_policy.h
@@ -2,6 +2,7 @@
 #include <list>
 #include <cstdint>
 #include <cstddef>
+#include <functional>
 
 class Segment;
 
@@ -66,6 +67,25 @@ public:
         return {};
     }
 
+    /* Same score-order scan as get_ghost_sum_for_free_segments, but instead of
+     * page sums it reports "victim v" — the newest segment (max create_timestamp)
+     * GC must touch to free `target_free_segments`.  Boundary segment is folded
+     * in whole (WT is atomic). Used as the upper bound of a later WT query. */
+    struct VictimWtSpanResult {
+        uint64_t max_wt = 0;            // "victim v": largest create_timestamp in cleaned set
+        double   m      = 0.0;          // segments folded in (boundary inclusive)
+        Segment* v_seg  = nullptr;      // the max-WT victim segment
+    };
+    virtual VictimWtSpanResult get_victim_wt_span_for_free_segments(double target_free_segments) const {
+        return {};
+    }
+
+    /* Visit victim segments in selection (score) order.  fn returns false to
+     * stop early.  Default no-op for policies without an ordered victim list.
+     * Used by what-if GC simulation that needs per-victim segment access
+     * (the heap is private to the concrete policy). */
+    virtual void for_each_victim_in_order(const std::function<bool(Segment*)>& fn) const {}
+
     /* Get current segment count in the policy */
     virtual size_t segment_count() const { return 0; }
 
diff --git a/evict_policy_cost_benefit.cpp b/evict_policy_cost_benefit.cpp
index 85dcdee..d598fd6 100644
--- a/evict_policy_cost_benefit.cpp
+++ b/evict_policy_cost_benefit.cpp
@@ -120,3 +120,35 @@ CbEvictPolicy::get_ghost_sum_for_free_segments(double target_free_segments) cons
     }
     return r;
 }
+
+EvictPolicy::VictimWtSpanResult
+CbEvictPolicy::get_victim_wt_span_for_free_segments(double target_free_segments) const
+{
+    VictimWtSpanResult r;
+    if (target_free_segments <= 0.0 || heap_.empty()) return r;
+
+    double free_sum = 0.0;
+    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
+        Segment* s = it->seg;
+        const uint64_t v   = s->valid_cnt;
+        const uint64_t inv = (pages_in_segment > v) ? (pages_in_segment - v) : 0;
+        const double inv_frac = static_cast<double>(inv) / static_cast<double>(pages_in_segment);
+
+        // Segment is part of the cleaned set → track the newest WT (victim v).
+        const uint64_t wt = s->get_create_time();
+        if (r.v_seg == nullptr || wt > r.max_wt) { r.max_wt = wt; r.v_seg = s; }
+        r.m += 1.0;
+
+        if (free_sum + inv_frac >= target_free_segments) break;  // target reached
+        free_sum += inv_frac;
+    }
+    return r;
+}
+
+void CbEvictPolicy::for_each_victim_in_order(const std::function<bool(Segment*)>& fn) const
+{
+    // max→min score order == the order choose_segment() would hand out victims.
+    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
+        if (!fn(it->seg)) break;
+    }
+}
diff --git a/evict_policy_cost_benefit.h b/evict_policy_cost_benefit.h
index 2fd58c0..8f85479 100644
--- a/evict_policy_cost_benefit.h
+++ b/evict_policy_cost_benefit.h
@@ -43,6 +43,8 @@ public:
     uint64_t get_mth_score_valid_pages(double m) const override;
     uint64_t get_kth_segment_valid_cnt_for_free_segments(double m) const override;
     GhostSumResult get_ghost_sum_for_free_segments(double target_free_segments) const override;
+    VictimWtSpanResult get_victim_wt_span_for_free_segments(double target_free_segments) const override;
+    void for_each_victim_in_order(const std::function<bool(Segment*)>& fn) const override;
 private:
     /* 실제 점수 계산: age/u  (u==0 → ∞) */
     inline double score(Segment* s) const {
diff --git a/istream.h b/istream.h
index 9552447..fdd03e3 100644
--- a/istream.h
+++ b/istream.h
@@ -5,6 +5,11 @@
 class IStream {
 public:
     virtual int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) = 0;
+    // Mutation-free twin of Classify() for what-if GC simulation. Returns the
+    // SAME stream id Classify() would, but must NOT touch any internal cycle /
+    // granularity bookkeeping (mStreamCycles, pending-victim queues, …). Default
+    // -1 = unsupported → caller should fall back to a single GC stream.
+    virtual int  ClassifyReadOnly(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) const { return -1; }
     virtual void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) = 0;
     virtual void GcAppend(uint64_t blockAddr) = 0;
     virtual void CollectSegment(Segment *segment, uint64_t global_timestamp) = 0;
diff --git a/log_cache.cpp b/log_cache.cpp
index a3d58b0..35dcd5a 100644
--- a/log_cache.cpp
+++ b/log_cache.cpp
@@ -83,7 +83,8 @@ LogCache::LogCache(uint64_t              cold_capacity,
       flush_ghost_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
       gg_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
       ff_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
-      gf_flush_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS))
+      gf_flush_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
+      lambda_ratio(MovingAverageRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS))
 {
     periodic_ratio_ = periodic_ratio;
     segment_size_blocks = cfg_.segment_bytes / blk_sz;
@@ -174,6 +175,7 @@ void LogCache::invalidate(long key, int lba_sz) {
             invalidate_blocks += 1;
             loc.seg->blocks[loc.idx].valid = false;
             --loc.seg->valid_cnt;
+            loc.seg->note_invalidation();   // per-seg cumulative inval count (++ only)
             global_valid_blocks -= 1;
             record_inv_time(key);
             if (loc.seg->full()){
@@ -362,6 +364,103 @@ void LogCache::update_ghost_compacted_blocks_sum_cum() {
     }
 }
 
+LogCache::GcNetFreeSim LogCache::simulate_gc_net_free() const
+{
+    GcNetFreeSim r;
+    const std::size_t seg_blocks = segment_size_blocks;
+    if (!compactor || seg_blocks == 0) return r;
+
+    // Per-stream simulated GC active segment: remaining free slots + running WT.
+    // Seeded from the live gc_active_seg so each one's current write_ptr counts
+    // (a victim whose valid pages fit in the leftover space allocates nothing).
+    struct SimSeg { std::size_t remaining; uint64_t wt; };
+    std::unordered_map<int, SimSeg> sim;
+    sim.reserve(gc_active_seg.size() * 2 + 8);
+    for (const auto& kv : gc_active_seg) {
+        LogCacheSegment* s   = kv.second;
+        const std::size_t used = s->write_ptr;
+        const std::size_t rem  = (seg_blocks > used) ? (seg_blocks - used) : 0;
+        sim[kv.first] = SimSeg{ rem, s->create_timestamp };
+    }
+
+    int      victims       = 0;
+    int      new_allocs    = 0;
+    uint64_t min_victim_wt = UINT64_MAX;
+    uint64_t active_min_wt = UINT64_MAX;
+
+    compactor->for_each_victim_in_order([&](Segment* base) -> bool {
+        LogCacheSegment* v = static_cast<LogCacheSegment*>(base);
+        ++victims;
+        if (v->create_timestamp < min_victim_wt) min_victim_wt = v->create_timestamp;
+
+        // Relocate every valid block, routing by its own create_timestamp.
+        uint64_t this_victim_seg_wt = UINT64_MAX;  // min WT of seg(s) THIS victim lands in
+        for (std::size_t i = 0; i < v->blocks.size(); ++i) {
+            const auto& blk = v->blocks[i];
+            if (!blk.valid) continue;
+            int sid = stream_policy
+                ? stream_policy->ClassifyReadOnly(blk.key, /*isGcAppend=*/true,
+                                                  log_cache_timestamp, blk.create_timestamp)
+                : -1;
+            if (sid < 0) sid = Segment::GC_STREAM_START;   // fallback: single GC stream
+
+            auto it = sim.find(sid);
+            if (it == sim.end()) {
+                // No live active seg for this stream → first GC write allocates one.
+                ++new_allocs;
+                it = sim.emplace(sid, SimSeg{ seg_blocks, log_cache_timestamp }).first;
+            } else if (it->second.remaining == 0) {
+                // Active seg full → allocate a fresh one for this stream.
+                ++new_allocs;
+                it->second = SimSeg{ seg_blocks, log_cache_timestamp };
+            }
+            SimSeg& ss = it->second;
+            --ss.remaining;
+            if (blk.create_timestamp < ss.wt) ss.wt = blk.create_timestamp;  // oldest-WT rule
+            if (ss.wt < this_victim_seg_wt) this_victim_seg_wt = ss.wt;
+            if (ss.wt < active_min_wt)       active_min_wt      = ss.wt;
+        }
+
+        // Victim segment is reclaimed (+1 free); allocations consumed free segs.
+        // net_free rises by at most 1 per victim, so the first time it reaches 1
+        // it is exactly 1 — that victim is the boundary.
+        if (victims - new_allocs >= 1) {
+            r.reached            = true;
+            r.victims            = victims;
+            r.new_allocs         = new_allocs;
+            r.boundary_victim_wt = v->create_timestamp;
+            r.boundary_seg_wt    = this_victim_seg_wt;
+            r.min_victim_wt      = min_victim_wt;
+            r.active_seg_min_wt  = active_min_wt;
+            return false;                            // stop scanning
+        }
+        return true;
+    });
+
+    if (!r.reached) {            // heap exhausted before reaching net_free==1
+        r.victims           = victims;
+        r.new_allocs        = new_allocs;
+        r.min_victim_wt     = min_victim_wt;
+        r.active_seg_min_wt = active_min_wt;
+    }
+    return r;
+}
+
+double LogCache::sum_invalidate_rate_in_wt_range(uint64_t wt_hi) const
+{
+    double sum = 0.0;
+    if (!evictor) return sum;
+    // evictor (score_age_evict = -create_timestamp, max-heap) hands out segments
+    // in WT-ascending order → once we pass wt_hi every later seg is also out.
+    // No lower bound: accumulate every resident seg up to victim v (wt_hi).
+    evictor->for_each_victim_in_order([&](Segment* s) -> bool {
+        if (s->get_create_time() > wt_hi) return false;   // ascending → done
+        sum += s->invalidate_rate();
+        return true;
+    });
+    return sum;
+}
+
 void LogCache::periodic_gs_predict_track() {
     // Phase 1: emit G(u+δ_k) for δ_k ∈ {(segs-1), segs, (segs+1)} every
     // segment/4 tick, buffer with timestamp.  When an entry has aged δ_k
@@ -581,7 +680,7 @@ void LogCache::periodic_ghost_delta_gc_sum_final() {
     //   * Gud = G(u+δ) per host write (cum_valid is prediction-only marginal cost).
     if (!is_ghost_cache) return;
 
-    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
+    if (log_cache_timestamp % segment_size_blocks == 0) {
         update_ghost_compacted_blocks_sum_cum();
         compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
         compaction_ratio_in_ghost_cache.updateFromCumulative(
@@ -595,8 +694,18 @@ void LogCache::periodic_ghost_delta_gc_sum_final() {
         // and denominator are in pages → EWMA value = fraction (0~1).
         flush_avg_ratio.updateFromCumulative(
             flush_event_count_ * segment_size_blocks, evicted_blocks);
+        // compact_avg = per-NET-FREE-segment valid copy. denom = victims·seg −
+        //   compacted_blocks = (segs GC reclaimed − segs GC re-allocated)·seg =
+        //   NET segments freed by GC ·seg. value = u/(1−u), unit-matched to the
+        //   ghost_sum per-free prediction. (Was per-victim u, which ignored that
+        //   GC re-writes valid into fresh segs → undercounted real free cost.)
         compact_avg_ratio.updateFromCumulative(
-            compact_event_count_ * segment_size_blocks, compacted_blocks);
+            compact_event_count_ * segment_size_blocks - compacted_blocks, compacted_blocks);
+        // λ: device blocks invalidated per host-write page. invalidate_blocks =
+        //   cumulative host-overwrite/trim invalidations; log_cache_timestamp =
+        //   cumulative host-write pages. EWMA over the moving_avg window → recent
+        //   per-page invalidation rate (×seg = blocks invalidated per host-seg).
+        lambda_ratio.updateFromCumulative(log_cache_timestamp, invalidate_blocks);
         // F_inv: rate at which ghost-resident pages die, measured AT POP time.
         //   totalPush − totalPop = Σ_popped(push_valid − pop_valid) + resident
         //   push. push_valid−pop_valid = pages a segment lost while resident in
@@ -639,6 +748,16 @@ void LogCache::periodic_ghost_delta_gc_sum_final() {
             gf_flush_ratio.updateFromCumulative(
                 log_cache_timestamp, static_cast<uint64_t>(gf_flush_sum_));
         }
+        // Per-segment invalidation rate: fold each resident (sealed) segment's
+        // cumulative count at this regular cadence — same updateFromCumulative
+        // pattern as the ratios above, so invrate_sum reads a recency-weighted
+        // rate while the per-event path stays a bare ++. ~O(sealed segs) per tick.
+        if (evictor) {
+            evictor->for_each_victim_in_order([this](Segment* s) -> bool {
+                s->fold_invalidate_rate(log_cache_timestamp);
+                return true;
+            });
+        }
     }
     if (log_cache_timestamp % segment_size_blocks == 0) {
         if (compaction_ratio.has_value() &&
@@ -658,26 +777,36 @@ void LogCache::periodic_ghost_delta_gc_sum_final() {
                                       ? flush_ghost_ratio.value() : 0.0;
             (void)F_ghost_rate;  // kept for logging / easy −F_ghost restore
             (void)F_pred_rate;   // kept for logging (col f_frac_pred)
-            // ── Marginal next-step comparison (de-confounds util level) ──
-            // Reuse the δN vs 2δN breadths as forward differences:
-            //   G  = Gud = ghost_sum(δN)    GG = ghost_sum(2δN)
-            //   F  = get_mth(δN)            FF = get_mth(2δN)
-            //   Gnext = GG − G  (cost of the 2nd GC step)
-            //   Fnext = FF − F  (cost of the 2nd flush step)
-            // Compare GC's escalation against flush's de-escalation; flush legs
-            // carry r·waf (cold-tier write cost):
-            //   LHS = Gnext − G          = GG − 2·G
-            //   RHS = r·waf·(F − Fnext)  = r·waf·(2F − FF)
-            // do G (RAISE) if LHS < RHS, else flush (LOWER).
-            const double rwaf  = periodic_ratio_ * waf_w;
+            // ── invrate-rule: GC if its copy cost beats the natural-death credit ──
+            //   invrate_sum = Σ per-seg invalidate_rate over resident segs with
+            //   WT ≤ v, where victim v = newest seg GC must touch to free
+            //   util_step_·N. Each invalidate_rate ≈ invalidated pages per
+            //   host-write page on that seg → the sum is the host-page death rate
+            //   of the old cohort GC would reclaim. Routing that reclaim through
+            //   cold-tier flush costs invrate_sum·r writes; GC instead copies Gud
+            //   valid pages. → GC (RAISE) iff Gud < invrate_sum·r, else flush.
+            //   Computed every tick (cheap heap scans); gcsim below stays log-only.
+            const double vic_target_free =
+                util_step_ * static_cast<double>(total_segments);
+            EvictPolicy::VictimWtSpanResult vspan;
+            double invrate_sum = 0.0;
+            if (compactor) {
+                vspan = compactor->get_victim_wt_span_for_free_segments(vic_target_free);
+                invrate_sum = sum_invalidate_rate_in_wt_range(vspan.max_wt);
+            }
+            const double lambda = lambda_ratio.has_value() ? lambda_ratio.value() : 0.0;
+            (void)lambda;  // kept for the `lambda` log column only (no longer in the rule)
+            const double lhs    = Gud;                           // GC copy cost / host page
+            const double rhs    = invrate_sum * periodic_ratio_; // invrate·r / host page
+            bool         raise  = (lhs < rhs);
+            // Marginal cols kept for logging only (not used by the λ-rule).
             const double GG    = gg_ratio.has_value()       ? gg_ratio.value()       : 0.0;
-            const double FFv   = ff_ratio.has_value()       ? ff_ratio.value()       : 0.0; // get_mth(2δN)
-            const double Fv    = gf_flush_ratio.has_value() ? gf_flush_ratio.value() : 0.0; // get_mth(δN)
+            const double FFv   = ff_ratio.has_value()       ? ff_ratio.value()       : 0.0;
+            const double Fv    = gf_flush_ratio.has_value() ? gf_flush_ratio.value() : 0.0;
             const double Gnext = GG  - Gud;
             const double Fnext = FFv - Fv;
-            const double lhs   = Gnext - Gud;          // GG − 2·Gud
-            const double rhs   = rwaf * (Fv - Fnext);  // r·waf·(2F − FF)
-            const bool   raise = (lhs < rhs);
+            // (anti-stuck cap removed: decision is now purely Gud < invrate_sum·r,
+            //  no forced flush after N consecutive RAISEs.)
             const double cur_util = (total_cache_block_count > 0)
                                   ? (double)global_valid_blocks / total_cache_block_count : 0.0;
             const double prev_target = target_valid_blk_rate;
@@ -705,15 +834,26 @@ void LogCache::periodic_ghost_delta_gc_sum_final() {
                             "ts segs r waf G_u F_u G_ud F_ud LHS RHS decision "
                             "cur_util tgt_before tgt_after hard_limit low_floor "
                             "comp_cum evict_cum ex_low_tgt ex_tgt_sat ex_force_flush ex_high_valid "
-                            "compact_avg flush_avg compact_evt ghost_compact_sum f_frac_pred GGr FFr Fr Gnext Fnext\n");
+                            "compact_avg flush_avg compact_evt ghost_compact_sum f_frac_pred GGr FFr Fr Gnext Fnext lambda "
+                            "gcsim_reached gcsim_m gcsim_alloc gcsim_vic_wt gcsim_seg_wt gcsim_minvic_wt gcsim_actmin_wt "
+                            "vic_v_wt invrate_sum\n");
                     }
                 }
             }
             if (g_gs_dec_fp) {
+                // What-if GC net-free sim — run only when the decision log is on
+                // (block-level victim scan is not free; keeps normal runs intact).
+                const GcNetFreeSim gcsim = simulate_gc_net_free();
+                auto wt_or_0 = [](uint64_t w) -> uint64_t {
+                    return (w == UINT64_MAX) ? 0UL : w;
+                };
+                // vspan / invrate_sum already computed above (now drive the rule).
                 std::fprintf(g_gs_dec_fp,
                     "%lu %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %s "
                     "%.6f %.6f %.6f %d %d %lu %lu %lu %lu %lu %lu "
-                    "%.6f %.6f %lu %.0f %.6f %.6f %.6f %.6f %.6f %.6f\n",
+                    "%.6f %.6f %lu %.0f %.6f %.6f %.6f %.6f %.6f %.6f %.6f "
+                    "%d %d %d %lu %lu %lu %lu "
+                    "%lu %.6f\n",
                     log_cache_timestamp, gs_decision_period_segs_,
                     periodic_ratio_, waf_w, Gu, Fu, Gud, Fud, lhs, rhs,
                     raise ? "RAISE" : "LOWER",
@@ -724,7 +864,11 @@ void LogCache::periodic_ghost_delta_gc_sum_final() {
                     g_ex_force_flush_count, g_ex_high_valid_victim_count,
                     compact_avg, flush_avg,
                     compact_event_count_, ghost_compacted_blocks_sum_,
-                    F_pred_rate, GG, FFv, Fv, Gnext, Fnext);
+                    F_pred_rate, GG, FFv, Fv, Gnext, Fnext, lambda,
+                    gcsim.reached ? 1 : 0, gcsim.victims, gcsim.new_allocs,
+                    gcsim.boundary_victim_wt, wt_or_0(gcsim.boundary_seg_wt),
+                    wt_or_0(gcsim.min_victim_wt), wt_or_0(gcsim.active_seg_min_wt),
+                    vspan.max_wt, invrate_sum);
                 std::fflush(g_gs_dec_fp);
             }
         }
diff --git a/log_cache.h b/log_cache.h
index 45203e4..9be204e 100644
--- a/log_cache.h
+++ b/log_cache.h
@@ -145,6 +145,32 @@ private:
     void update_ghost_compacted_blocks_sum();
     void update_ghost_compacted_blocks_sum_cum();  // Final: tick-based cum_valid accumulator
 
+    // ── What-if GC net-free simulation ───────────────────────────────────
+    // Walk the live compactor victim list in score order, relocating each
+    // victim's valid blocks into per-stream GC active segments (seeded from the
+    // current gc_active_seg write pointers, routed via stream_policy's
+    // mutation-free ClassifyReadOnly), until one NET segment is freed:
+    //   net_free = victims_consumed - gc_active_segs_allocated == 1.
+    // Reports the boundary victim's WT (create_timestamp) and the resulting
+    // active-seg WT under the GC "oldest-WT wins" rule. Pure read-only — mutates
+    // NO live state (LogCache, compactor heap, or stream_policy cycle state).
+    struct GcNetFreeSim {
+        bool     reached            = false;        // net_free==1 actually reached
+        int      victims            = 0;            // m: victims consumed
+        int      new_allocs         = 0;            // new gc active segs allocated
+        uint64_t boundary_victim_wt = 0;            // m-th victim seg create_timestamp
+        uint64_t boundary_seg_wt    = UINT64_MAX;   // min WT of active seg(s) the m-th victim landed in
+        uint64_t min_victim_wt      = UINT64_MAX;   // min create_timestamp over consumed victims
+        uint64_t active_seg_min_wt  = UINT64_MAX;   // min WT across all touched active segs
+    };
+    GcNetFreeSim simulate_gc_net_free() const;
+
+    // Σ invalidate_rate over every evictor (WT-ordered) segment with
+    // create_timestamp <= wt_hi. O(rank) scan with early-stop (evictor is WT
+    // ascending). Pair with get_victim_wt_span_for_free_segments by passing
+    // span.max_wt → prefix invalidate-rate mass up to and including victim v.
+    double sum_invalidate_rate_in_wt_range(uint64_t wt_hi) const;
+
     /* trace(optional) *****************************************************/
     bool  cache_trace_;
     FILE* trace_fp_      = nullptr;
@@ -221,6 +247,10 @@ private:
     double    ff_flush_sum_ = 0.0;
     EwmaRatio gf_flush_ratio;    // get_mth(δN)  (GF's flush leg; GC leg = Gud)
     double    gf_flush_sum_ = 0.0;
+    // λ: device blocks invalidated per host-write page (EWMA over moving_avg
+    //    window = 1 host-seg = 한 host_writes).
+    //    GS_FINAL λ-rule: GC (RAISE) if Gud < λ·r, else flush.
+    EwmaRatio lambda_ratio;
     double periodic_ratio_ = 2.88;
     EwmaRatio ghost_util_ratio;  // ghost miss rate = U(util_step)
     GhostCache ghost_cache;
@@ -433,5 +463,6 @@ public:
         gg_ratio                        = MovingAverageRatio::Make(type, window_blocks);
         ff_ratio                        = MovingAverageRatio::Make(type, window_blocks);
         gf_flush_ratio                  = MovingAverageRatio::Make(type, window_blocks);
+        lambda_ratio                    = MovingAverageRatio::Make(type, window_blocks);
     }
 };
diff --git a/log_cache_segment.h b/log_cache_segment.h
index f3b64c1..63caada 100644
--- a/log_cache_segment.h
+++ b/log_cache_segment.h
@@ -27,10 +27,11 @@ public:
 
     /* helpers */
     inline bool full()  override  { return write_ptr >= blocks.size(); }
-    inline void reset() override 
+    inline void reset() override
     {
         write_ptr = 0;
         valid_cnt = 0;
         for (auto &b : blocks) b.valid = false;
+        reset_invalidate_rate();
     }
 };
diff --git a/multi_hot_cold.cpp b/multi_hot_cold.cpp
index 763d2d9..f8beb7b 100644
--- a/multi_hot_cold.cpp
+++ b/multi_hot_cold.cpp
@@ -71,6 +71,35 @@ int MultiHotCold::Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_
     return stream_id + Segment::GC_STREAM_START;
 }
 
+int MultiHotCold::ClassifyReadOnly(uint64_t /*blockAddr*/, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) const {
+    // Pure computation twin of Classify(): returns the same stream id WITHOUT
+    // mutating any state. Specifically it never writes mTimestampGranularity,
+    // g_cycle_length, mStreamCycles, g_stream_cycles or mPendingVictimStreams —
+    // so it is safe to call repeatedly inside a what-if GC simulation.
+    uint64_t gran = static_cast<uint64_t>(mTimestampGranularity);
+    uint64_t fresh_interval = compute_stream_interval(0);
+    if (fresh_interval > 0) gran = fresh_interval;   // local copy only, not stored
+    if (gran == 0) gran = 1;                          // div-by-zero guard
+
+    uint64_t time_diff = global_timestamp - created_timestamp;
+    if (!isGcAppend) {
+        uint64_t lifespan = time_diff;
+        return (lifespan != 0 && lifespan < mAvgLifespan) ? 0 : 1;
+    }
+    if (mCheckCreatedTimestampOnly) {
+        time_diff = created_timestamp;
+    }
+    int raw_id = static_cast<int>(time_diff / gran);
+    int stream_id;
+    if (mCheckCreatedTimestampOnly) {
+        stream_id = raw_id % mMaxGcStreams;           // no cycle-wrap bookkeeping
+    } else {
+        stream_id = raw_id;
+        if (stream_id >= mMaxGcStreams) stream_id = mMaxGcStreams - 1;
+    }
+    return stream_id + Segment::GC_STREAM_START;
+}
+
 int MultiHotCold::GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) {
     if (!mCheckCreatedTimestampOnly) return -1;
 
diff --git a/multi_hot_cold.h b/multi_hot_cold.h
index e556f56..938b5ab 100644
--- a/multi_hot_cold.h
+++ b/multi_hot_cold.h
@@ -13,6 +13,7 @@ class MultiHotCold: public IStream {
 public:
     MultiHotCold(int max_gc_streams, int timestamp_granularity, bool check_created_timestamp_only, bool classify_for_host_append = false, bool classfy_for_gc_append = true, int num_host_streams = 2);
     int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) override;
+    int  ClassifyReadOnly(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) const override;
     void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) override;
     void GcAppend(uint64_t blockAddr){};
     void CollectSegment(Segment *segment, uint64_t global_timestamp) override;
diff --git a/segment.h b/segment.h
index 5ba4eb3..e17f10c 100644
--- a/segment.h
+++ b/segment.h
@@ -2,6 +2,7 @@
 #include <vector>
 #include <cstddef>
 #include <cstdint>
+#include "emwa.h"
 
 /**
  * 한 세그먼트(≈32 MB)를 구성하는 내부 자료구조
@@ -18,9 +19,45 @@ public:
     int class_num = 0;
     bool hot = false;
     uint64_t create_timestamp;
+    // Host-invalidation rate, in invalidations per host-write page. Each host
+    // overwrite/trim of a resident block bumps invalidate_count_ (note_invalidation,
+    // O(1)); fold_invalidate_rate() differentiates that count against host time at
+    // GS ticks and EWMAs it (same updateFromCumulative pattern as the other ratios).
+    // Summed over a WT band at decision time to gauge the old cohort's death rate.
+    static constexpr double INVAL_RATE_ALPHA = 0.1;
+    uint64_t invalidate_count_      = 0;   // cumulative host-invalidations on this seg
+    uint64_t invalidate_prev_count_ = 0;   // count at last fold
+    uint64_t invalidate_prev_ts_    = 0;   // host-time at last fold (0 = unseeded)
+    Ewma     invalidate_ewma_{INVAL_RATE_ALPHA};   // shared kernel (emwa.h), fixed-α
     /* helpers */
     virtual bool full() = 0;
     virtual void reset() = 0;
+    // Current invalidation rate (invalidations per host-write page), EWMA-smoothed.
+    double invalidate_rate() const {
+        return invalidate_ewma_.has_value() ? invalidate_ewma_.value() : 0.0;
+    }
+    // Per-event hot path: just bump the cumulative count (O(1), no float work).
+    void note_invalidation() { ++invalidate_count_; }
+    // Periodic fold (called at GS ticks, same updateFromCumulative pattern as the
+    // other ratios): differentiate the cumulative count against host time, EWMA
+    // the per-tick rate. First call seeds the baseline (no rate emitted yet).
+    void fold_invalidate_rate(uint64_t now) {
+        if (invalidate_prev_ts_ == 0) {
+            invalidate_prev_ts_    = now;
+            invalidate_prev_count_ = invalidate_count_;
+            return;
+        }
+        if (now <= invalidate_prev_ts_) return;            // need dH > 0
+        const uint64_t dH = now - invalidate_prev_ts_;
+        const uint64_t dU = invalidate_count_ - invalidate_prev_count_;
+        invalidate_prev_ts_    = now;
+        invalidate_prev_count_ = invalidate_count_;
+        invalidate_ewma_.update(static_cast<double>(dU) / static_cast<double>(dH));
+    }
+    void reset_invalidate_rate() {
+        invalidate_ewma_.reset();
+        invalidate_count_ = invalidate_prev_count_ = invalidate_prev_ts_ = 0;
+    }
     virtual uint64_t get_create_time() {
         return create_timestamp;
     }
```
