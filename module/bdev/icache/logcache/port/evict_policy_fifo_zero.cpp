#include "evict_policy_fifo_zero.h"
#include <cassert>
#include "log_cache_segment.h"
#include <cstdio>

/* ───────── add ───────── */
void FifoZeroEvictPolicy::add(Segment* seg)
{
    assert(seg);
    if (seg->valid_cnt == 0) {
        auto it = zero_queue.insert(zero_queue.end(), seg);
        handle[seg] = it;
    } else {
        auto it = queue.insert(queue.end(), seg);
        handle[seg] = it;
    }
}

/* ───────── remove ───────── */
void FifoZeroEvictPolicy::remove(Segment* seg)
{
    auto it = handle.find(seg);
    if (it == handle.end()) return;

    /* 어떤 큐에 있든 erase 가능 */
    queue.erase(it->second);
    zero_queue.erase(it->second);
    handle.erase(it);
}

/* ───────── update ─────────
 * valid_cnt 변경(0 ↔︎ 0 아님) 시 큐 이동 */
void FifoZeroEvictPolicy::update(Segment* seg)
{
    auto it = handle.find(seg);
    if (it == handle.end()) return;           // 미추적 세그먼트
    bool is_zero = (seg->valid_cnt == 0);

    /* 현재 위치가 올바른 큐인지 확인 */
    if (is_zero)
    {
        /* → zero_queue 로 이동 */
        auto node_it = it->second;
        queue.erase(node_it);
        auto new_it  = zero_queue.insert(zero_queue.end(), seg);
        it->second   = new_it;
    }
}
/* ───────── choose_segment ───────── */
Segment* FifoZeroEvictPolicy::choose_segment()
{
    /* 1) valid_cnt == 0 인 세그먼트가 있으면 먼저 선택 */
    if (!zero_queue.empty()) {
        Segment* vic = zero_queue.front();
        zero_queue.pop_front();
        assert (vic->valid_cnt == 0);
        // check vic->bitmap
        // LogCache Segment 로 치환해서, 모든 blocK이 valid=false 인지 확인
        auto log_cache_seg = dynamic_cast<LogCacheSegment*>(vic);
        if (log_cache_seg && log_cache_seg->write_ptr > 0) {
            // 모든 블록이 valid=false 인지 확인
            for (const auto& block : log_cache_seg->blocks) {
                if (block.valid) {
                    assert(false); // 모든 블록이 valid=false 여야 함
                }
            }
        }
        handle.erase(vic);
        return vic;
    }

    /* 2) 그 외엔 일반 FIFO */
    if (queue.empty()) return nullptr;
    Segment* vic = queue.front();
    queue.pop_front();
    assert (vic->valid_cnt > 0);
    handle.erase(vic);
    return vic;
}
