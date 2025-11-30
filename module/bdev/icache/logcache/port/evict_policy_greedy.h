#pragma once
#include "evict_policy.h"
#include <boost/heap/d_ary_heap.hpp>
#include <unordered_map>

struct HeapNode_Cache {
    std::size_t cnt;
    Segment*    seg;

    // cnt 가 작을수록 우선순위 ↑
    bool operator>(const HeapNode_Cache& o) const noexcept {
        return cnt > o.cnt || (cnt == o.cnt && seg > o.seg);
    }
};

// C++11 호환
using Heap = boost::heap::d_ary_heap<
                 HeapNode_Cache,
                 boost::heap::arity<4>,
                 boost::heap::mutable_<true>,
                 boost::heap::compare<std::greater<HeapNode_Cache>>
>;

class GreedyEvictPolicy : public EvictPolicy {
public:
    Segment* choose_segment() override;

    void add   (Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override;

private:
    Heap heap_;
    std::unordered_map<Segment*, Heap::handle_type> handle_;
};