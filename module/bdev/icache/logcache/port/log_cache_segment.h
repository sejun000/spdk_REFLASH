#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include "segment.h"

// Stripe width: number of zones per segment for parallel writes
#define STRIPE_WIDTH 2
// Stripe chunk size in blocks (128KB = 32 * 4KB blocks)
#define STRIPE_CHUNK_BLOCKS 32

/**
 * 한 세그먼트를 구성하는 내부 자료구조
 * With striping: 1 segment = STRIPE_WIDTH zones
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

    explicit LogCacheSegment(std::size_t blocks_per_segment, uint64_t create_timestamp,
                             int stripe_width = STRIPE_WIDTH)
        : Segment(create_timestamp), blocks(blocks_per_segment), stripe_width_(stripe_width) {
        physical_bases.resize(stripe_width, 0);
    }

    /* data */
    std::vector<Block> blocks;
    std::vector<uint64_t> physical_bases;  // N zone bases for striping
    int stripe_width_ = STRIPE_WIDTH;
    uint64_t zone_capacity_bytes_ = 0;     // capacity per zone (for offset calculation)

    // Legacy single-zone compatibility
    uint64_t physical_base = 0;

    // Get physical offset for a block index (striped across zones in 128KB chunks)
    uint64_t get_block_offset(std::size_t idx, uint64_t block_size) const {
        if (stripe_width_ <= 1) {
            // No striping - legacy mode
            return physical_bases[0] + idx * block_size;
        }
        // Striped: round-robin 128KB chunks across zones
        std::size_t chunk_idx = idx / STRIPE_CHUNK_BLOCKS;
        int zone_idx = chunk_idx % stripe_width_;
        std::size_t offset_in_chunk = idx % STRIPE_CHUNK_BLOCKS;
        std::size_t block_in_zone = (chunk_idx / stripe_width_) * STRIPE_CHUNK_BLOCKS + offset_in_chunk;
        return physical_bases[zone_idx] + block_in_zone * block_size;
    }

    // Get zone index for a block (128KB chunk based)
    int get_zone_idx(std::size_t idx) const {
        std::size_t chunk_idx = idx / STRIPE_CHUNK_BLOCKS;
        return chunk_idx % stripe_width_;
    }

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
