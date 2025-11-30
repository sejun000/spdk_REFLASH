#include "evict_policy_selective_fifo.h"
#include "log_cache_segment.h"
#include <cassert>
#include <stdio.h>

void SelectiveFifoEvictPolicy::add(Segment* seg)
{
    assert(seg);
    int stream_id = seg->get_class_num();
    assert(stream_id >= 0 && stream_id < MAX_QUEUE_SIZE);
    auto it = queue[stream_id].insert(queue[stream_id].end(), seg);  // push_back
    g_valid_cnt[stream_id] += seg->valid_cnt;
    handle[seg] = it;
}

void SelectiveFifoEvictPolicy::remove(Segment* seg)
{
    auto it = handle.find(seg);
    if (it == handle.end()) return;            // 이미 제거됨
    int stream_id = seg->get_class_num();
    assert(stream_id >= 0 && stream_id < MAX_QUEUE_SIZE);
    queue[stream_id].erase(it->second);                   // O(1) 삭제
    g_valid_cnt[stream_id] -= seg->valid_cnt;
    handle.erase(it);
}

void SelectiveFifoEvictPolicy::update(Segment* seg)
{
    int stream_id = seg->get_class_num();
    g_valid_cnt[stream_id]--;
}  

Segment* SelectiveFifoEvictPolicy::choose_segment()
{
    if (reverse == false) {
        for (int i = MAX_QUEUE_SIZE - 1; i >= 0; --i) {
            if (!queue[i].empty()) {
                Segment* victim = queue[i].front();
                queue[i].pop_front();
                handle.erase(victim);
                return victim;  // 가장 높은 우선순위의 victim 반환
            }
        }
    }
    else {
        static int status_index = 0;
        if (status_index % 100 == 0) {
            for (int i = 0; i < 10; ++i) {
                printf("%d %ld\n", i, queue[i].size());
            }
        }
        status_index +=1;
        uint64_t loop_idx = 0;
        for (int i = 0; i < MAX_QUEUE_SIZE; ++i) {
          int seq_id = i;
          if (seq != nullptr && !gc) {
            if (seq->size() <= (size_t)i) {
                break;
            }
            seq_id = (*seq)[i];
         }
          /*  if (gc == true && g_valid_cnt[i] > queue[i].size() * pages_in_segment * 0.95){
                printf("why? %ld", g_valid_cnt[i]);
                continue;
            }*/
           //printf("%d %d\n", i, queue[i].size());
            if (!queue[seq_id].empty()) {
                Segment* victim = queue[seq_id].front();
                queue[seq_id].pop_front();
                if (victim->valid_cnt > pages_in_segment * 0.85 && gc) {
                    auto it = queue[seq_id].insert(queue[seq_id].end(), victim);  // push_back
                    handle[victim] = it;
                    if (loop_idx >= queue[seq_id].size()) {
                        loop_idx = 0;
                        continue;
                    }
                    i -= 1;
                    loop_idx++;
                    continue;
                }
                handle.erase(victim);
                assert (victim != nullptr);
                return victim;  // 가장 높은 우선순위의 victim 반환
            }
            loop_idx = 0;
        }
    }
    assert (false);
    return nullptr;  // 모든 큐가 비어있음
}

Segment* SelectiveFifoEvictPolicy::choose_segment(int stream_id)
{
    for (int i = stream_id; i >= 0; --i) {
        assert(stream_id >= 0 && stream_id < MAX_QUEUE_SIZE);
        if (queue[stream_id].empty()) return nullptr;  // 해당 스트림의 큐가 비어있음
        Segment* victim = queue[stream_id].front();
        queue[stream_id].pop_front();
        handle.erase(victim);
        return victim;  // 해당 스트림에서 가장 오래된 세그먼트 반환
    }
    return nullptr;
}