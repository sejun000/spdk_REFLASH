#include <iostream>
#include <cfloat>
#include "sepbit.h"
#include "fifo.h"
#include "metadata.h"

SepBIT::SepBIT() {
    mAvgLifespan = DBL_MAX;
    mClassNumOfLastCollectedSegment = 0;
    mLba2Fifo = new FIFO();
    mMetadata = new Metadata();
}

int SepBIT::Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) {
  static uint64_t hot = 0, cold = 0;
  if (!isGcAppend) {
    uint64_t lifespan = mLba2Fifo->Query(blockAddr);
    if (lifespan != UINT64_MAX && lifespan < mAvgLifespan) {
     // printf("0 Classify: %lu, lifespan: %lu, avg lifespan: %f\n", blockAddr, lifespan, mAvgLifespan);
      hot++;
      return 0;
    } else {
     // printf("1 Classify: %lu, lifespan: %lu, avg lifespan: %f %lu %lu\n", blockAddr, lifespan, mAvgLifespan, hot, cold++);
      cold++;
      return 1;
    }
  } else {
    if (mClassNumOfLastCollectedSegment == 0) {
      return 2 + Segment::GC_STREAM_START;
    } else {
      uint64_t age = global_timestamp - mMetadata->Query(blockAddr);
      if (age < 4 * mAvgLifespan) {
        return 3 + Segment::GC_STREAM_START;
      } else if (age < 16 * mAvgLifespan) {
        return 4 + Segment::GC_STREAM_START;
      } else {
        return 5 + Segment::GC_STREAM_START;
      }
    }
  }
}

void SepBIT::CollectSegment(Segment *segment, uint64_t global_timestamp) {
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

  mClassNumOfLastCollectedSegment = segment->get_class_num();
}

void SepBIT::Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) {
  uint64_t valid_blocks = reinterpret_cast<uint64_t>(arg);
  global_timestamp += 1;
  mLba2Fifo->Update(blockAddr, mAvgLifespan, valid_blocks);
  mMetadata->Update(blockAddr, global_timestamp);
}

void SepBIT::GcAppend(uint64_t blockAddr) {
}
