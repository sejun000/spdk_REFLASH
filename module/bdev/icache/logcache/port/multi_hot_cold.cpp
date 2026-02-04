#include "multi_hot_cold.h"
#include <cfloat>

MultiHotCold::MultiHotCold(int max_gc_streams, int timestamp_granularity, bool check_created_timestamp_only, bool classify_for_host_append, bool classfy_for_gc_append){
    mMaxGcStreams = max_gc_streams;
    mTimestampGranularity = timestamp_granularity;
    mCheckCreatedTimestampOnly = check_created_timestamp_only;
    mClassifyForHostAppend = classify_for_host_append;
    mClassifyForGcAppend = classfy_for_gc_append;
    mAvgLifespan = DBL_MAX;
}

extern uint64_t g_threshold;

int MultiHotCold::Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) {
    uint64_t time_diff = global_timestamp - created_timestamp;
    if (!isGcAppend) {
        uint64_t lifespan = global_timestamp - created_timestamp;
        if (lifespan != 0 && lifespan < mAvgLifespan) {
            return 0;
        }
        else {
            return 1;
        }
    }
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

void MultiHotCold::CollectSegment(Segment *segment, uint64_t global_timestamp) {
  static uint64_t totLifespan = 0;
  static int nCollects = 0;
  if (segment->get_class_num() == 0) {
    //printf("CollectSegment: %lu, class_num: %d\n mAvgLifespan %f\n", segment->get_create_time(), segment->get_class_num(), mAvgLifespan);
    totLifespan += global_timestamp - segment->get_create_time();
    nCollects += 1;
  }
  if (nCollects == 16) {
    mAvgLifespan = 1.0 * totLifespan / nCollects;
    nCollects = 0;
    totLifespan = 0;
    //std::cout << "AvgLifespan: " << mAvgLifespan << std::endl;
  }
}