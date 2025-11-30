#pragma once

#include "evict_policy.h"
#include "istream.h"
#include "segment.h"
#include <cstddef>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

/**
 * MiDASGreedyEvictPolicy
 *  - Approximates MiDAS' greedy victim selection while honouring the
 *    per-(channel,die) stream separation performed by MiDASHotCold.
 *  - Each stream keeps its own min-heap ordered by the number of valid blocks.
 *    We walk the stream list in a round-robin fashion to promote wear-level
 *    and parallel GC similar to MiDAS.
 */
class MiDASGreedyEvictPolicy : public EvictPolicy {
public:
    explicit MiDASGreedyEvictPolicy(std::size_t stream_count = IStream::MAX_STREAMS);

    void add(Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override;
    Segment* choose_segment() override;

private:
    struct QueueEntry {
        uint64_t  valid_cnt;
        uint64_t  generation;
        Segment*  seg;
        int       stream;
    };

    struct Compare {
        bool operator()(const QueueEntry& a, const QueueEntry& b) const {
            if (a.valid_cnt != b.valid_cnt) {
                return a.valid_cnt > b.valid_cnt;
            }
            return a.generation > b.generation;
        }
    };

    using Queue = std::priority_queue<QueueEntry, std::vector<QueueEntry>, Compare>;

    std::size_t normalise_stream(int class_num) const;
    void        push_entry(Segment* seg, std::size_t stream);

    std::vector<Queue> queues_;
    std::unordered_map<Segment*, std::pair<uint64_t, std::size_t>> version_;
    uint64_t     generation_counter_;
    std::size_t  next_stream_;
};
