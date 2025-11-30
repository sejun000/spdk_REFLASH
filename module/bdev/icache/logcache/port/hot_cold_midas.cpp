#include "hot_cold_midas.h"
#include <algorithm>

namespace {
uint32_t clamp_host_streams(uint32_t requested) {
    return std::max<uint32_t>(1, std::min<uint32_t>(requested, IStream::MAX_STREAMS));
}

uint32_t clamp_gc_streams(uint32_t requested) {
    if (Segment::GC_STREAM_START >= IStream::MAX_STREAMS) {
        return 1;
    }
    uint32_t available = IStream::MAX_STREAMS - Segment::GC_STREAM_START;
    return std::max<uint32_t>(1, std::min<uint32_t>(requested, available));
}
} // namespace

MiDASHotCold::MiDASHotCold(uint32_t channels, uint32_t chips_per_channel)
    : channels_(std::max<uint32_t>(1, channels)),
      chips_per_channel_(std::max<uint32_t>(1, chips_per_channel)),
      host_streams_(clamp_host_streams(channels_ * chips_per_channel_)),
      gc_streams_(clamp_gc_streams(channels_ * chips_per_channel_)) {}

int MiDASHotCold::Classify(uint64_t blockAddr,
                           bool isGcAppend,
                           uint64_t /*global_timestamp*/,
                           uint64_t /*created_timestamp*/) {
    if (isGcAppend) {
        return gc_stream_id(blockAddr);
    }
    return host_stream_id(blockAddr);
}

void MiDASHotCold::Append(uint64_t /*blockAddr*/,
                          uint64_t /*global_timestamp*/,
                          void*    /*arg*/) {
    // MiDAS host allocation does not need per-LBA bookkeeping in this model.
}

void MiDASHotCold::GcAppend(uint64_t /*blockAddr*/) {
    // Nothing to do; GC stream selection is stateless.
}

void MiDASHotCold::CollectSegment(Segment* /*segment*/,
                                  uint64_t /*global_timestamp*/) {
    // No-op for MiDAS default behaviour.
}

int MiDASHotCold::host_stream_id(uint64_t blockAddr) const {
    return static_cast<int>(blockAddr % host_streams_);
}

int MiDASHotCold::gc_stream_id(uint64_t blockAddr) const {
    uint32_t idx = static_cast<uint32_t>(blockAddr % gc_streams_);
    return Segment::GC_STREAM_START + static_cast<int>(idx);
}
