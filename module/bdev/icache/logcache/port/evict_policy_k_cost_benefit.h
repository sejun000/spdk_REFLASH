/*───────────────────────────────────────────────────────*/
/* cb_evict_policy_ost.h – K‑th Cost‑Benefit eviction    */
/*   header (PBDS order‑statistic tree)                  */
/*───────────────────────────────────────────────────────*/
#pragma once

#include "evict_policy.h"
#include <ext/pb_ds/assoc_container.hpp>
#include <ext/pb_ds/tree_policy.hpp>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <cmath>
#include "segment.h"

extern std::atomic<uint64_t> logical_time;   // 페이지 단위 · 전역 증가

/* ───── internal node ───── */
struct CbItem {
    double    score;   // age/u
    Segment*  seg;

    /* 내림차순 정렬: score가 클수록 "앞" */
    bool operator<(const CbItem& o) const noexcept {
        return score > o.score ||                 // 큰 score → 먼저
              (score == o.score && seg < o.seg);  // tie‑breaker
    }
};

using OSTBase = __gnu_pbds::tree<
    CbItem,
    __gnu_pbds::null_type,
    std::less<CbItem>,
    __gnu_pbds::rb_tree_tag,
    __gnu_pbds::tree_order_statistics_node_update>;

class KthCbEvictPolicy final : public EvictPolicy {
public:
    using RankFunc  = std::function<std::size_t(std::size_t)>; // ranking(N)
    using ScoreFunc = std::function<double(Segment*)>;          // 외부 score()

    explicit KthCbEvictPolicy(ScoreFunc sf = nullptr,
                              RankFunc  rf = nullptr);

    /* EvictPolicy 인터페이스 */
    void add   (Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override;
    Segment* choose_segment() override;
    Segment* choose_segment(bool next_id) override;
    Segment* choose_segment(size_t idx);

private:
    double score(Segment* s) const;     // 기본 age/u 계산

    OSTBase ost_;
    std::unordered_map<Segment*, OSTBase::iterator> h_;
    ScoreFunc score_func_;
    RankFunc  rank_func_;
    std::size_t last_evicted_idx = 0;
};
