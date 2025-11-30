#pragma once
#include <cstdint>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cassert>
#include <cstring>

class Metadata
{
  public:
    Metadata()
    {
        mArray = new uint64_t[kSize];
        memset(mArray, 0, kSize * sizeof(uint64_t));
    }

    void Update(uint64_t offset, uint64_t meta)
    {
      offset /= 4096;
      mArray[offset] = meta;
    }

    uint64_t Query(uint64_t offset)
    {
      uint64_t meta;
      offset /= 4096;
      meta = mArray[offset];
      return meta;
    }
    uint64_t* mArray;
    const uint64_t kSize = 512ull * 1024 * 1024 * 1024 / 4096;
};
