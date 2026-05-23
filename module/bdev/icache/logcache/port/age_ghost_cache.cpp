#include "age_ghost_cache.h"

AgeGhostCache::AgeGhostCache(std::size_t capacity_segs)
    : capacity_segs_(capacity_segs)
{}

void AgeGhostCache::pushSegment(const std::vector<uint64_t>& blocks)
{
    if (capacity_segs_ == 0) return;

    segs_.push_back(GhostSeg{static_cast<uint64_t>(blocks.size()), blocks});
    total_valid_count_ += blocks.size();

    SegIt it = std::prev(segs_.end());
    for (uint64_t b : blocks) {
        block_to_seg_[b] = it;
    }

    while (segs_.size() > capacity_segs_) {
        evict_oldest();
    }
}

bool AgeGhostCache::invalidate(uint64_t block_id)
{
    auto mit = block_to_seg_.find(block_id);
    if (mit == block_to_seg_.end()) return false;

    SegIt sit = mit->second;
    if (sit->valid_count > 0) {
        --sit->valid_count;
        --total_valid_count_;
    }
    block_to_seg_.erase(mit);
    return true;
}

void AgeGhostCache::setCapacity(std::size_t capacity_segs)
{
    capacity_segs_ = capacity_segs;
    while (segs_.size() > capacity_segs_) {
        evict_oldest();
    }
}

void AgeGhostCache::reset()
{
    segs_.clear();
    block_to_seg_.clear();
    total_valid_count_ = 0;
}

void AgeGhostCache::evict_oldest()
{
    if (segs_.empty()) return;

    GhostSeg& front = segs_.front();
    if (total_valid_count_ >= front.valid_count) {
        total_valid_count_ -= front.valid_count;
    } else {
        total_valid_count_ = 0;
    }
    for (uint64_t b : front.blocks) {
        block_to_seg_.erase(b);
    }
    segs_.pop_front();
}
