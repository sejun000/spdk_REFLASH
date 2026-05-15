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

uint64_t CbEvictPolicy::get_mth_score_valid_pages(double m) const
{
    if (m <= 0.0) {
        return 0;
    }
    int full = static_cast<int>(m);       // e.g. 2
    double frac = m - full;               // e.g. 0.4
    uint64_t sum = 0;
    int count = 0;
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        if (count < full) {
            sum += it->seg->valid_cnt;
        } else if (frac > 0.0) {
            sum += static_cast<uint64_t>(it->seg->valid_cnt * frac);
            ++count;
            break;
        } else {
            break;
        }
        ++count;
    }
    return sum;
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

EvictPolicy::GhostSumResult
CbEvictPolicy::get_ghost_sum_for_free_segments(double target_free_segments) const
{
    GhostSumResult r;
    if (target_free_segments <= 0.0 || heap_.empty()) return r;

    double free_sum = 0.0;
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        const uint64_t v   = it->seg->valid_cnt;
        const uint64_t inv = (pages_in_segment > v) ? (pages_in_segment - v) : 0;
        const double inv_frac = static_cast<double>(inv) / static_cast<double>(pages_in_segment);
        if (free_sum + inv_frac >= target_free_segments) {
            const double need = target_free_segments - free_sum;
            const double frac = (inv > 0)
                              ? (need * static_cast<double>(pages_in_segment) / static_cast<double>(inv))
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
