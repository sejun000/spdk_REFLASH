#include "multi_hot_cold.h"

MultiHotCold::MultiHotCold(int max_gc_streams, int timestamp_granularity, bool check_created_timestamp_only, bool classify_for_host_append, bool classfy_for_gc_append){
    mMaxGcStreams = max_gc_streams;
    mTimestampGranularity = timestamp_granularity;
    mCheckCreatedTimestampOnly = check_created_timestamp_only;
    mClassifyForHostAppend = classify_for_host_append;
    mClassifyForGcAppend = classfy_for_gc_append;
}

extern uint64_t g_threshold;

int MultiHotCold::Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) {
    uint64_t time_diff = global_timestamp - created_timestamp;
    if (mCheckCreatedTimestampOnly) {
        time_diff = created_timestamp;
    }
    int stream_id = time_diff / mTimestampGranularity;
    if (stream_id >= mMaxGcStreams) {
        if (mCheckCreatedTimestampOnly)
        {
            stream_id = stream_id % mMaxGcStreams;
        }
        else 
        {
            stream_id = mMaxGcStreams - 1; // Limit to max GC streams
        }
    }
    // Implement classification logic here
    // For now, we return 0 as a placeholder
    auto it = oldest_timestamp_map.find(stream_id);
    if (it == oldest_timestamp_map.end()) {
        // If found, use the stored timestamp
        oldest_timestamp_map[stream_id] = created_timestamp;
    }
    else if (oldest_timestamp_map[stream_id] ) {
        // If not found, update the timestamp
        if (created_timestamp < it->second) {
            oldest_timestamp_map[stream_id] = created_timestamp;
        }
    }
    else {
        // If not found, initialize it
        oldest_timestamp_map[stream_id] = created_timestamp;
    }
    return stream_id + Segment::GC_STREAM_START;
}

int MultiHotCold::GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) {
    if (!mCheckCreatedTimestampOnly) return -1;
    for (int index = 0; index < mMaxGcStreams; index++) {
        auto it = oldest_timestamp_map.find(index);
        if (it == oldest_timestamp_map.end()) {
            continue;
        }
        if (global_timestamp - oldest_timestamp_map[index] >= threshold) {
            int retId = index + Segment::GC_STREAM_START;
            oldest_timestamp_map.erase(index);
            return retId;
        }
    }
    return -1;
}