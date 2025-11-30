#include "evict_policy_multiqueue.h"
#include "log_cache_segment.h"
#include <cassert>
#include <stdio.h>

void MultiQueueEvictPolicy::add(Segment* seg)
{
    assert(seg);
}

void MultiQueueEvictPolicy::add(Segment* seg, uint64_t current_time)
{
    assert(seg);
    int queue_id = get_queue_id(current_time - seg->get_create_time());
    assert(queue_id >= 0 && queue_id < MAX_QUEUE_SIZE);
    auto it = queue[queue_id].insert(queue[queue_id].end(), seg);
    g_valid_cnt[queue_id] += seg->valid_cnt;
    handle[seg] = {it, queue_id};
}

void MultiQueueEvictPolicy::remove(Segment* seg)
{
    auto it = handle.find(seg);
    if (it == handle.end()) return;            // 이미 제거됨
    int queue_id = it->second.queue_id;
    assert(queue_id >= 0 && queue_id < MAX_QUEUE_SIZE);
    queue[queue_id].erase(it->second.it);      // O(1) 삭제
    g_valid_cnt[queue_id] -= seg->valid_cnt;
    handle.erase(it);
}

void MultiQueueEvictPolicy::update(Segment* seg)
{
    auto it = handle.find(seg);
    if (it == handle.end()) return;            // 이미 제거됨
    int queue_id = it->second.queue_id;
    g_valid_cnt[queue_id]--;
}

Segment* MultiQueueEvictPolicy::choose_segment()
{
    for (int i = MAX_QUEUE_SIZE - 1; i >= 0; --i) {
        if (!queue[i].empty()) {
            Segment* victim = queue[i].front();
            queue[i].pop_front();
            handle.erase(victim);
            return victim;  // 가장 높은 우선순위의 victim 반환
        }
    }
    assert(false);
    return nullptr;  // 모든 큐가 비어있음
}