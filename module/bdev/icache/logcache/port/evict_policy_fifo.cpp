#include "evict_policy_fifo.h"
#include "log_cache_segment.h"
#include <cassert>

void FifoEvictPolicy::add(Segment* seg)
{
    assert(seg);
    auto it = queue.insert(queue.end(), seg);  // push_back
    handle[seg] = it;
}

void FifoEvictPolicy::remove(Segment* seg)
{
    auto it = handle.find(seg);
    if (it == handle.end()) {
        //assert(false);
        return;            // 이미 제거됨
    }
    queue.erase(it->second);                   // O(1) 삭제
    handle.erase(it);
}

Segment* FifoEvictPolicy::choose_segment()
{
    if (queue.empty()) return nullptr;
    Segment* victim = queue.front();
    queue.pop_front();
    handle.erase(victim);
    return victim;
}