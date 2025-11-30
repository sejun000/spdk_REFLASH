#include "evict_policy_greedy.h"
#include "log_cache_segment.h"
#include <cassert>

void GreedyEvictPolicy::add(Segment* seg)
{
    assert(seg);
    Heap::handle_type h = heap_.push({ seg->valid_cnt, seg });
    handle_[seg] = h;
}

void GreedyEvictPolicy::remove(Segment* seg)
{
    auto it = handle_.find(seg);
    if (it == handle_.end()) return;          // 이미 빠졌다면 무시
    heap_.erase(it->second);
    handle_.erase(it);
}

void GreedyEvictPolicy::update(Segment* seg)
{
    auto it = handle_.find(seg);
    if (it == handle_.end()) { 
    //    add(seg); 
        return; 
    }

    HeapNode_Cache newNode{ seg->valid_cnt, seg };
    heap_.update(it->second, newNode);        // decrease/increase-key 모두 OK
}

Segment* GreedyEvictPolicy::choose_segment()
{
    if (heap_.empty()) return nullptr;
    return heap_.top().seg;
}
