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
};