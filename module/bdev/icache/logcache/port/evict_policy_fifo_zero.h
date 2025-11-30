#pragma once
#include "evict_policy.h"
#include <list>
#include <unordered_map>
#include "segment.h"

class FifoZeroEvictPolicy : public EvictPolicy {
public:
    /* EvictPolicy API */
    void add   (Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override;     // valid_cnt 변화 감지
    Segment* choose_segment() override;

private:
    std::list<Segment*>                   queue;       // 일반 FIFO
    std::list<Segment*>                   zero_queue;  // valid_cnt == 0
    std::unordered_map<Segment*,
        std::list<Segment*>::iterator>    handle;      // seg → iterator
};
