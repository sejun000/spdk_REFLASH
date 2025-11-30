#pragma once
#include "evict_policy.h"
#include <list>
#include <unordered_map>

class FifoEvictPolicy : public EvictPolicy {
public:
    /* EvictPolicy API */
    void add   (Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override {}   // FIFO는 no-op
    Segment* choose_segment() override;

private:
    std::list<Segment*> queue;   // 삽입 순서 그대로 유지
    std::unordered_map<Segment*,
                       std::list<Segment*>::iterator> handle; // seg→iterator
};