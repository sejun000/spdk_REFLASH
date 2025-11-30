#include "ghost_cache.h"

GhostCache::GhostCache(std::size_t capacity)
    : capacity_(capacity), evict_count_(0) {}

void GhostCache::reset() {
    cache_.clear();
    lookup_.clear();
    evict_count_ = 0;
}

bool GhostCache::access(uint64_t block_id) {
    auto it = lookup_.find(block_id);
    if (it != lookup_.end()) {
        // hit → 리스트에서 제거 + 맵에서도 제거
        cache_.erase(it->second);
        lookup_.erase(it);
        return true;
    }
    return false;
}


bool GhostCache::push(uint64_t block_id) {
    auto it = lookup_.find(block_id);
    if (it != lookup_.end()) {
        // hit → 리스트에서 제거 + 맵에서도 제거
        cache_.erase(it->second);
        lookup_.erase(it);
        return true;
    }

    // miss
    if (cache_.size() >= capacity_) {
        // FIFO eviction (front)
        uint64_t evicted = cache_.front();
        cache_.pop_front();
        lookup_.erase(evicted);
        evict_count_++;
    }

    // 새 block 추가 (back)
    cache_.push_back(block_id);
    lookup_[block_id] = std::prev(cache_.end());
    return false;
}
