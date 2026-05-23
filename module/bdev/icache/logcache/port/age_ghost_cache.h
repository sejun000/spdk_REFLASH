#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

// Segment-granular FIFO ghost cache for GS_FINAL (PrGh) policy.
// Capacity is in number of segments (D = gs_decision_period_segs_).
// pushSegment(keys) appends a new segment with valid_count = keys.size().
// invalidate(key) decrements the owning segment's valid_count by 1.
// totalValidCount() returns the sum of valid_count across all retained segs.
class AgeGhostCache {
public:
    explicit AgeGhostCache(std::size_t capacity_segs);

    void     pushSegment(const std::vector<uint64_t>& blocks);
    bool     invalidate(uint64_t block_id);

    uint64_t totalValidCount() const { return total_valid_count_; }

    void     setCapacity(std::size_t capacity_segs);
    void     reset();

private:
    struct GhostSeg {
        uint64_t              valid_count;
        std::vector<uint64_t> blocks;
    };
    using SegIt = std::list<GhostSeg>::iterator;

    void evict_oldest();

    std::size_t                         capacity_segs_;
    std::list<GhostSeg>                 segs_;
    std::unordered_map<uint64_t, SegIt> block_to_seg_;
    uint64_t                            total_valid_count_ = 0;
};
