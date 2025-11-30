/*───────────────────────────────────────────────────────*/
/* cb_evict_policy_ost.cpp – implementation              */
/*───────────────────────────────────────────────────────*/
#include "evict_policy_k_cost_benefit.h"   // 같은 파일 이름 유지
#include <limits>
#include <utility>
#include <cassert>

/*──────────────────────── helpers ─────────────────────*/

double KthCbEvictPolicy::score(Segment* s) const
{
    if (score_func_)
        return score_func_(s);

    if (logical_time == nullptr)
        throw std::runtime_error("logical_time is not set");

    const uint64_t age = *logical_time - s->get_create_time();
    if (s->valid_cnt == 0)
        return std::numeric_limits<double>::infinity();

    const double u = static_cast<double>(s->valid_cnt) / pages_in_segment;
    return age / (u + 1e-5);   // divide‑by‑zero guard
}

/*──────────────────── ctor ───────────────────────────*/
KthCbEvictPolicy::KthCbEvictPolicy(ScoreFunc sf, RankFunc rf)
    : score_func_(std::move(sf)),
      rank_func_(std::move(rf)) {}

/*───────────────── mutators ─────────────────────────*/
void KthCbEvictPolicy::add(Segment* s)
{
    assert(s->full());
    double score_value = score(s);
    auto it2 = h_.find(s);
    if (it2 != h_.end()) {
        // after gc, invalidate
        return;
    }
    
    if (score_value < -(int64_t)UINT64_MAX + 1)
    {
        return;
    }
    auto it = ost_.insert({ score(s), s }).first;
    h_[s] = it;
}

void KthCbEvictPolicy::remove(Segment* s)
{
    auto it = h_.find(s);
    if (it == h_.end()) return;
    ost_.erase(it->second);
    h_.erase(it);
}

void KthCbEvictPolicy::update(Segment* s)
{
    assert(s->full());
    auto hit = h_.find(s);
    //if (hit == h_.end()) return;          // already removed
    if (hit == h_.end()) {
        add(s);
        hit = h_.find(s);
        if (hit == h_.end()) return;
    }
    CbItem node = *(hit->second);         // copy old node
    ost_.erase(hit->second);              // erase from tree
    
    node.score = score(s);                // recompute score
    auto new_it   = ost_.insert(node).first;
    hit->second = new_it;                 // refresh handle
}

/*────────────────── choose_segment ───────────────────*/

Segment* KthCbEvictPolicy::choose_segment(std::size_t idx) {

    // 2) nth element iterator 가져오기
    Segment* victim = nullptr;
    
    auto it = ost_.find_by_order(idx);
    if (it == ost_.end()) return nullptr;
    // 3) victim 저장
    victim = it->seg;
        
    auto it2 = h_.find(victim);
    
    assert (it2 != h_.end()) ;
    // 4) remove()로 완전 제거
    remove(victim);
    
    assert(victim->full());
    last_evicted_idx = idx;
    // 5) 반환
    return victim;
}

Segment* KthCbEvictPolicy::choose_segment()
{
    if (ost_.empty()) return nullptr;

    // 1) 인덱스 결정
    std::size_t idx = 0;
    if (rank_func_) {
        idx = rank_func_(ost_.size());
        if (idx >= ost_.size()) idx = ost_.size() - 1;
    }

    return choose_segment(idx);
}



Segment* KthCbEvictPolicy::choose_segment(bool next_id)
{
    if (next_id == false || last_evicted_idx == 0) {
        return choose_segment();
    }
    return choose_segment(last_evicted_idx - 1);
}
