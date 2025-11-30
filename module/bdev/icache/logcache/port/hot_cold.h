#pragma once

#include <cstdint>
#include <unordered_map>
#include "istream.h"

class FIFO;
class Metadata;

class HotCold: public IStream {
  public:
    HotCold();
    int  Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) override;
    void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) override;
    void GcAppend(uint64_t blockAddr) override;
    void CollectSegment(Segment *segment, uint64_t global_timestamp) override;

  private:

    std::unordered_map<uint64_t, uint64_t> creation_time_map;
    std::unordered_map<uint64_t, uint64_t> gc_time_map;
    double mAvgLifespan;
    FIFO* mLba2Fifo;
    Metadata* mMetadata;
    uint64_t mClassNumOfLastCollectedSegment;
};
