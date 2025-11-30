#include <iostream>
#include <cfloat>
#include "sepbit.h"
#include "fifo.h"
#include "metadata.h"
#include "hot_cold.h"

HotCold::HotCold() {
   
}

int HotCold::Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) {
  // We set global timerstamp as "time stamp diff"
  static int cold = 0;
  static int hot = 0;
  if (global_timestamp - created_timestamp <= 16 *1024ULL * 1024ULL / 4 && created_timestamp != UINT64_MAX) {
    //printf("hot %d, cold %d, global_timestamp %lu, created_timestamp %lu\n", hot, cold, global_timestamp, created_timestamp);
    hot++;
    return 0;
  }
  cold++;
  return 1;
}

void HotCold::CollectSegment(Segment *segment, uint64_t global_timestamp) {
    // Do notthing
}

void HotCold::Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) {
  
  // Do nothing
}

void HotCold::GcAppend(uint64_t blockAddr) {
}
