/*───────────────────────────────────────────────────────*/
/* cb_evict_policy.cpp                                   */
/*───────────────────────────────────────────────────────*/
#include "evict_policy_cost_benefit.h"
#include <cassert>

CbEvictPolicy::CbEvictPolicy(double (*func)(Segment* )) {
    score_func = func;
}

void CbEvictPolicy::add(Segment* s)
{
    assert(s);
    auto h = heap_.push({ score(s), s });
    h_[s]  = h;
}

void CbEvictPolicy::remove(Segment* s)
{
    auto it = h_.find(s);
    if (it == h_.end()) return;
    heap_.erase(it->second);
    h_.erase(it);
}

void CbEvictPolicy::update(Segment* s)
{
    auto it = h_.find(s);
    if (it == h_.end()) return;            // 이미 제거된 경우
    heap_.update(it->second, { score(s), s });
}

Segment* CbEvictPolicy::choose_segment()
{
    /* top‑k (K_VALIDATE) 노드 재평가로 순위 확정 */
    for (int i = 0; i < K_VALIDATE && !heap_.empty(); ++i) {
        CBNode top = heap_.top();
        double cur = score(top.seg);
        if (cur == top.score) {
            remove(top.seg);
            return top.seg;        // 여전히 1등
        }
        heap_.pop();                                 // 순위 달라짐
        auto h = heap_.push({ cur, top.seg });       // 재삽입
        h_[top.seg] = h;
    }
    return heap_.empty() ? nullptr : heap_.top().seg;
}
