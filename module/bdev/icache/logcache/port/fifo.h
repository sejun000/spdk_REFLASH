#pragma once
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cassert>
#include <map>
#include <cstring>
#include <cstdint>

class FIFO
{
  public:
    FIFO()
    {
        mArray = new uint64_t[kFileSize];
    }

    void Update(uint64_t blockAddr, double threshold, uint64_t num_valid_blocks)
    {
      double nValidBlocks = num_valid_blocks;
      //int res = 0;

      mArray[mTail] = blockAddr;
      mMap[blockAddr] = mTail;
      mTail += 1;
      if (mTail == kFileSize) mTail = 0;

      if ((mTail + kFileSize - mHead) % kFileSize > std::min(threshold, nValidBlocks))
      {
        uint64_t oldBlockAddr = mArray[mHead];
        if (mMap[oldBlockAddr] == mHead)
        {
          mMap.erase(oldBlockAddr);
        }
        mHead += 1;
        if (mHead == kFileSize) mHead = 0;

        if ((mTail + kFileSize - mHead) % kFileSize > threshold)
        {
          oldBlockAddr = mArray[mHead];

          if (mMap[oldBlockAddr] == mHead)
          {
            mMap.erase(oldBlockAddr);
          }
          mHead += 1;
          if (mHead == kFileSize) mHead = 0;
        }
      }
    }

    uint64_t Query(uint64_t blockAddr)
    {
      auto it = mMap.find(blockAddr);
      if (it == mMap.end())
      {
        return UINT32_MAX;
      }
      uint64_t position = it->second;
      uint64_t lifespan = (mTail < position) ?
        mTail + kFileSize - position : mTail - position;
      return lifespan;
    }

    uint64_t mTail = 0;
    uint64_t mHead = 0;
    std::map<uint64_t, uint64_t> mMap;
    uint64_t* mArray = NULL;
    const uint64_t kFileSize = 128 * 1024 * 1024;
};
