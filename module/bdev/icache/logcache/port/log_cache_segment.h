#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include "segment.h"

/**
 * 한 세그먼트(≈32 MB)를 구성하는 내부 자료구조
 */
class LogCacheSegment : public Segment
{
public:
    struct Block
    {
        struct {
            long key = 0;      ///< LBA(block) index (63비트)
            bool valid = false; ///< 유효성 플래그 (1비트)
        };
        uint64_t create_timestamp = UINT64_MAX; ///< 생성 시각
    };

    explicit LogCacheSegment(std::size_t blocks_per_segment, uint64_t create_timestamp)
        : Segment(create_timestamp), blocks(blocks_per_segment) {}

    /* data */
    std::vector<Block> blocks;
    uint64_t physical_base = 0;

    /* helpers */
    inline bool full()  override  { return write_ptr >= blocks.size(); }
    inline void reset() override 
    {
        write_ptr = 0;
        valid_cnt = 0;
        for (auto &b : blocks) {
            b.valid = false;
            b.create_timestamp = UINT64_MAX;
        }
    }
};
