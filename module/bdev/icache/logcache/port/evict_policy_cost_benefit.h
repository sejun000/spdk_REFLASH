/*───────────────────────────────────────────────────────*/
/* evict_policy_cb.h – Cost‑Benefit(age/u) Eviction      */
/*───────────────────────────────────────────────────────*/
#pragma once
#include "evict_policy.h"
#include <boost/heap/d_ary_heap.hpp>
#include <unordered_map>
#include <atomic>
#include <cmath>
#include "segment.h"


extern std::atomic<uint64_t> logical_time;   // 페이지 단위 · 전역 증가

struct CBNode {
    double    score;   // age / u  (cached)
    Segment*  seg;
    uint64_t* logical_time;
    /* 높은 score가 더 우선 ⇒ max‑heap */
    bool operator<(const CBNode& o) const noexcept {
        return score < o.score ||
              (score == o.score && seg < o.seg);
    }
};

using CBHeap = boost::heap::d_ary_heap<
    CBNode,
    boost::heap::arity<4>,
    boost::heap::mutable_<true>,
    boost::heap::compare<std::less<CBNode>>   // ← 여기만 변경
>;

class CbEvictPolicy final : public EvictPolicy {
public:
    /* EvictPolicy 인터페이스 구현 */
    Segment* choose_segment() override;
    CbEvictPolicy(double (*func)(Segment* ) = nullptr);
    void add   (Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override;
    bool empty() const override { return heap_.empty(); }
    size_t segment_count() const override { return heap_.size(); }
    uint64_t get_mth_score_valid_pages(double m) const override;
    uint64_t get_kth_segment_valid_cnt_for_free_segments(double m) const override;
private:
    /* 실제 점수 계산: age/u  (u==0 → ∞) */
    inline double score(Segment* s) const {
        if (score_func) {
            return score_func(s);
        }
        if (logical_time == nullptr) {
            throw std::runtime_error("logical_time is not set");
        }
        const uint64_t age = *logical_time - s->get_create_time();
        if (s->valid_cnt == 0) return std::numeric_limits<double>::infinity();
        const double u = static_cast<double>(s->valid_cnt) / pages_in_segment;
        return age / (u + 0.00001); // 0.00001: 0로 나누는 것 방지
    }
    static constexpr int K_VALIDATE = 10;   // top‑k 재검증
    CBHeap heap_;
    double (*score_func)(Segment* );
    std::unordered_map<Segment*, CBHeap::handle_type> h_;
};
