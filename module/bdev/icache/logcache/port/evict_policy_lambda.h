/*───────────────────────────────────────────────────────*/
/* evict_policy_lambda.h  – LambdaEvictPolicy            */
/*   score = 1 - exp[-λ (K - age)]                       */
/*   큰 score → benefit↑ (cold) → victim                 */
/*───────────────────────────────────────────────────────*/
#pragma once
#include "evict_policy.h"
#include "segment.h"
#include <boost/heap/d_ary_heap.hpp>
#include <unordered_map>
#include <deque>
#include <atomic>
#include <cmath>

/* ---------- 힙 노드 ---------- */
struct LambdaNode {
    double   score;         // 1 - S(K)/S(L)
    Segment* seg;
    bool operator<(const LambdaNode& o) const noexcept {
        return score < o.score || (score == o.score && seg < o.seg);
    }
};
using LambdaHeap = boost::heap::d_ary_heap<
                 LambdaNode,
                 boost::heap::arity<4>,
                 boost::heap::mutable_<true>,
                 boost::heap::compare<std::less<LambdaNode>>
             >;

/* ---------- LambdaEvictPolicy ---------- */
class LambdaEvictPolicy final : public EvictPolicy {
public:
    LambdaEvictPolicy(std::size_t K = 4);   // λ 추정창

    /* EvictPolicy 구현 */
    void add   (Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override;
    double score(Segment* seg);
    Segment* choose_segment() override;

private:

    /* 파라미터 */
    const std::size_t K_;
    static constexpr int K_VALIDATE = 4;

    /* 자료구조 */
    LambdaHeap heap_;
    std::unordered_map<Segment*, LambdaHeap::handle_type> h_;
    std::deque<double> hist_;   // λ 추정
    double lambda_ = 0.0;
};