#pragma once
#include "evict_policy.h"
#include <list>
#include <unordered_map>
#include <vector>
#include <memory>

struct HandleValue {
    std::list<Segment*>::iterator it;
    int queue_id;
};

class MultiQueueEvictPolicy : public EvictPolicy {
public:
    /* EvictPolicy API */
    MultiQueueEvictPolicy(uint64_t input_age_granularity = 0)
    {
        age_granularity = input_age_granularity;
    }
    void add   (Segment* seg) override;
    void add   (Segment* seg, uint64_t current_time) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override; // FIFO는 no-op
    int get_queue_id (uint64_t age) {
        if (age_granularity == 0) {
            return 0; // age granularity is not set, do nothing
        }
        int stream_id = age / age_granularity;
        if (stream_id >= MAX_QUEUE_SIZE) {
            stream_id = MAX_QUEUE_SIZE - 1; // cap to max queue size
        }
        return stream_id;
    }
    Segment* choose_segment() override;
    

private:
    static const int MAX_QUEUE_SIZE = 16; // 최대 큐 개수
    std::list<Segment*> queue[MAX_QUEUE_SIZE];   // 삽입 순서 그대로 유지
    std::unordered_map<Segment*, HandleValue> handle; // seg→iterator
    std::size_t        g_valid_cnt[MAX_QUEUE_SIZE];
    uint64_t age_granularity = 0; // age granularity for segment selection
};