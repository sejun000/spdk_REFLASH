#pragma once

#include <cstdint>
#include <vector>
#include "istream.h"
#include "fifo.h"
#include "metadata.h"

extern int g_stream_cycles[IStream::MAX_STREAMS];
extern uint64_t g_cycle_length;  // = granularity * max_gc_streams

class MultiHotCold: public IStream {
public:
    MultiHotCold(int max_gc_streams, int timestamp_granularity, bool check_created_timestamp_only, bool classify_for_host_append = false, bool classfy_for_gc_append = true, int num_host_streams = 2);
    int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) override;
    void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) override;
    void GcAppend(uint64_t blockAddr){};
    void CollectSegment(Segment *segment, uint64_t global_timestamp) override;
    int GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) override;
    int getNumHostStreams() const override { return mNumHostStreams; }
private:
    int mMaxGcStreams;
    int mTimestampGranularity;

    bool mCheckCreatedTimestampOnly;
    bool mClassifyForHostAppend; // If true, classify for host append
    bool mClassifyForGcAppend; // If true, classify for GC append
    double mAvgLifespan;
    int mStreamCycles[IStream::MAX_STREAMS];       // per-stream cycle number
    std::vector<int> mPendingVictimStreams;         // streams needing dummy fill due to cycle wrap
    int mNumHostStreams;
    FIFO* mLba2Fifo;
    Metadata* mMetadata;
};
