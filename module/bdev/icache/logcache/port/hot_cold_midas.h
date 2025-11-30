#pragma once

#include "hot_cold.h"
#include "segment.h"
#include <cstdint>
#include <unordered_map>

/**
 * MiDASHotCold
 *  - Emulates the MiDAS page-FTL behaviour of assigning per-parallel-unit
 *    active blocks for host writes and GC writes.
 *  - Host writes are mapped to streams in a deterministic round-robin manner
 *    across (channel, chip) pairs.
 *  - GC relocations reuse a disjoint stream namespace that begins at
 *    Segment::GC_STREAM_START so that LogCache can keep GC segments separate
 *    from host streams.
 */
class MiDASHotCold : public HotCold {
public:
    MiDASHotCold(uint32_t channels = 8, uint32_t chips_per_channel = 4);

    int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp,
                  uint64_t created_timestamp) override;
    void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) override;
    void GcAppend(uint64_t blockAddr) override;
    void CollectSegment(Segment *segment, uint64_t global_timestamp) override;

private:
    int host_stream_id(uint64_t blockAddr) const;
    int gc_stream_id(uint64_t blockAddr) const;

    const uint32_t channels_;
    const uint32_t chips_per_channel_;
    const uint32_t host_streams_;
    const uint32_t gc_streams_;
};
