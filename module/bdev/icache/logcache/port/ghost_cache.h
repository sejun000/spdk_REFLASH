#pragma once

#include <list>
#include <unordered_map>
#include <cstddef>
#include <cstdint>

class GhostCache {
public:
    explicit GhostCache(std::size_t capacity);

    // block 접근
    // return: true if hit, false if miss
    bool access(uint64_t block_id);
    bool push(uint64_t block_id);

    // eviction 횟수
    std::size_t evictCount() const { return evict_count_; }

    // g_u' = (push_count - access_hit_count) / push_count
    // ghost cache에 들어온 블록 중 다시 access 안 된 비율
    double utilization() const {
        if (push_count_ == 0) return 1.0;  // 아직 push 없으면 1.0 반환
        return static_cast<double>(push_count_ - access_hit_count_) / push_count_;
    }

    // 초기화
    void reset();

    // 현재 cache 크기
    std::size_t size() const { return cache_.size(); }

private:
    using ListIt = std::list<uint64_t>::iterator;

    std::size_t capacity_;
    std::list<uint64_t> cache_;  // FIFO: front=oldest, back=newest
    std::unordered_map<uint64_t, ListIt> lookup_;
    std::size_t evict_count_;
    std::size_t push_count_ = 0;        // push() 호출 횟수
    std::size_t access_hit_count_ = 0;  // access()에서 hit한 횟수
};