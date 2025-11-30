#pragma once
#include "evict_policy.h"
#include "istream.h"
#include <list>
#include <unordered_map>
#include <vector>
#include <memory>

class SelectiveFifoEvictPolicy : public EvictPolicy {
public:
    /* EvictPolicy API */
    SelectiveFifoEvictPolicy(bool input_reverse = false, bool input_gc = false, std::unique_ptr<std::vector<int>> input_seq = nullptr) :    
    reverse(input_reverse),
    gc(input_gc),
    seq(std::move(input_seq)){
    }
    void add   (Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override; // FIFO는 no-op
    Segment* choose_segment() override;
    Segment* choose_segment(int stream_id) override;

private:
    static const int MAX_QUEUE_SIZE = IStream::MAX_STREAMS * 2; // 최대 큐 개수
    std::list<Segment*> queue[MAX_QUEUE_SIZE];   // 삽입 순서 그대로 유지
    std::unordered_map<Segment*,
                       std::list<Segment*>::iterator> handle; // seg→iterator
    std::size_t        g_valid_cnt[MAX_QUEUE_SIZE];
    bool reverse = false;
    bool gc = false;
    std::unique_ptr<std::vector<int>> seq;
};