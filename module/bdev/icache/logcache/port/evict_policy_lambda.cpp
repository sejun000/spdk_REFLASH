/*───────────────────────────────────────────────────────*/
/* evict_policy_lambda.cpp                               */
/*───────────────────────────────────────────────────────*/
#include "evict_policy_lambda.h"
#include <cassert>
#include <numeric>

LambdaEvictPolicy::LambdaEvictPolicy(std::size_t K)
    :K_(K)
    {}


double LambdaEvictPolicy::score(Segment* s)
{
    uint64_t age_pages = *logical_time - s->get_create_time();
    double   v_now     = static_cast<double>(s->valid_cnt);

    /* λ_i: 블록별 평균 무효화율 (페이지 기준) */
    double   invalid = static_cast<double>(pages_in_segment - v_now);
    double   lambda  = (age_pages > 0) ? invalid / age_pages : 0.0;

    return -lambda;
}

/* ───── add / remove / update ───── */
void LambdaEvictPolicy::add(Segment* s)
{
    auto h = heap_.push({ score(s), s });
    h_[s]  = h;
}
void LambdaEvictPolicy::remove(Segment* s)
{
    auto it = h_.find(s);
    if (it == h_.end()) return;
    heap_.erase(it->second);
    h_.erase(it);
}
void LambdaEvictPolicy::update(Segment* s)
{
    auto it = h_.find(s);
    if (it == h_.end()) return;
    heap_.update(it->second, { score(s), s });
}

/* ───── choose_segment ───── */
Segment* LambdaEvictPolicy::choose_segment()
{
    for (std::size_t i = 0; i < K_ && !heap_.empty(); ++i) {
        LambdaNode top = heap_.top();
        double cur = score(top.seg);
        if (cur == top.score) return top.seg;   // 가장 benefit 높은 victim 확정
        heap_.pop();
        auto h = heap_.push({ cur, top.seg });
        h_[top.seg] = h;
    }
    return heap_.empty() ? nullptr : heap_.top().seg;
}