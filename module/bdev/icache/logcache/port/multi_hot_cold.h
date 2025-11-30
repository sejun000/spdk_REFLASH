#pragma once

#include <cstdint>
#include <unordered_map>
#include "istream.h"
class MultiHotCold: public IStream {
public:
    MultiHotCold(int max_gc_streams, int timestamp_granularity, bool check_created_timestamp_only, bool classify_for_host_append = false, bool classfy_for_gc_append = true);
    int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) override;
    void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg){};
    void GcAppend(uint64_t blockAddr){};
    void CollectSegment(Segment *segment, uint64_t global_timestamp){};
    int GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) override;
private:
    int mMaxGcStreams;
    int mTimestampGranularity;

    bool mCheckCreatedTimestampOnly;
    bool mClassifyForHostAppend; // If true, classify for host append
    bool mClassifyForGcAppend; // If true, classify for GC append
    std::unordered_map<int, uint64_t> oldest_timestamp_map; // Set of GC stream IDs

};
