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
    if (h_.count(s)) return;  // Already in heap, skip duplicate add
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
    size_t initial_size = heap_.size();

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

    // K_VALIDATE 반복 후에도 결정 안 됨 - 현재 top 선택
    if (heap_.empty()) {
        fprintf(stderr, "CbEvictPolicy::choose_segment: heap empty! initial_size=%zu\n", initial_size);
        assert(false);
    }
    Segment* seg = heap_.top().seg;
    remove(seg);
    return seg;
}

uint64_t CbEvictPolicy::get_mth_score_valid_pages(int m) const
{
    if (m <= 0) {
        return 0;
    }
    // ordered_iterator로 score 순서대로 순회하며 1~2m번째 평균 valid_cnt 계산
    int search_limit = m * 2;
    uint64_t sum = 0;
    int count = 0;
    auto it = heap_.ordered_begin();
    for (int i = 0; i < search_limit && it != heap_.ordered_end(); ++i, ++it) {
        sum += it->seg->valid_cnt;
        ++count;
    }
    if (count == 0) {
        return 0;
    }
    return sum / count;
}

uint64_t CbEvictPolicy::get_kth_segment_valid_cnt_for_free_segments(double m) const
{
    if (m <= 0.0 || heap_.empty()) return 0;

    double free_sum = 0.0;
    uint64_t last_valid_cnt = 0;

    // score 순서(max→min)로 순회, (1-U_i) 누적하여 m 이상이 되면 마지막 segment의 valid_cnt 반환
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        double u_i = static_cast<double>(it->seg->valid_cnt) / pages_in_segment;
        free_sum += (1.0 - u_i);
        last_valid_cnt = it->seg->valid_cnt;
        if (free_sum >= m) break;
    }

    return last_valid_cnt;
}
