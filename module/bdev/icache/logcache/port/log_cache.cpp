#include "log_cache.h"
#include "../log_cache_config.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <list>
#include <memory>

#include "spdk/log.h"

extern uint64_t interval;
/* ------------------------------------------------------------------ */
/* ctor / dtor                                                        */
/* ------------------------------------------------------------------ */
LogCache::LogCache(uint64_t              cold_capacity,
             uint64_t              cache_block_count,
             int                   blk_sz,
             bool                  cache_trace,
             const std::string&    trace_file,
             const std::string&    cold_trace,
             std::string&    waf_log_file,
             std::unique_ptr<EvictPolicy> ev,
             const Config*         cfg, 
             IStream *input_stream_policy,
             double input_target_valid_blk_rate,
             std::unique_ptr<EvictPolicy> cp,
             double input_additional_free_blks_ratio_by_gc,
             bool input_ghost_cache,
             std::string stat_log_file,
             CacheDeviceInterface *device_io
             )
    : ICache(cold_capacity, waf_log_file, stat_log_file),
      cache_block_size(blk_sz),
      cfg_(cfg ? *cfg : Config{}),
      evictor(std::move(ev)),
      cache_trace_(cache_trace),
      target_valid_blk_rate(input_target_valid_blk_rate),
      valid_blk_rate_hard_limit(0.93),
      compactor(std::move(cp)),
      additional_free_blks_ratio_by_gc(input_additional_free_blks_ratio_by_gc),
      evicted_ages_histogram(std::make_unique<Histogram>("evicted_ages", cache_block_count * 2 / HISTOGRAM_BUCKETS, HISTOGRAM_BUCKETS * 2, fp_stats)),
      evicted_blocks_histogram(std::make_unique<Histogram>("evicted_blocks", 400, HISTOGRAM_BUCKETS, fp_stats)),
      compacted_blocks_histogram(std::make_unique<Histogram>("compacted_blocks", 400, HISTOGRAM_BUCKETS, fp_stats)),
      evicted_ages_with_segment_histogram(std::make_unique<Histogram>("evicted_ages_with_segment", cache_block_count * 2 / HISTOGRAM_BUCKETS, HISTOGRAM_BUCKETS * 2, fp_stats)),
      compacted_ages_with_segment_histogram(std::make_unique<Histogram>("compacted_ages_with_segment", cache_block_count * 2 / HISTOGRAM_BUCKETS, HISTOGRAM_BUCKETS * 2, fp_stats)),
      evicted_cache_blocks_per_evict(std::make_unique<Histogram>("evicted_cache_blocks_per_evict", 1, 100, fp_stats)),
      is_ghost_cache(input_ghost_cache),
      compaction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio_in_ghost_cache(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      compaction_ratio_in_ghost_cache(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_cache(cache_block_count * GHOST_CACHE_RATIO),
      device_io_(device_io)
{
    // For striping: segment_bytes = zone_capacity * stripe_width
    // Each segment spans stripe_width zones
    int stripe_width = cfg_.stripe_width;
    std::size_t zone_capacity = cfg_.zone_capacity_bytes > 0 ? cfg_.zone_capacity_bytes : cfg_.segment_bytes;
    std::size_t zone_size = cfg_.zone_size_bytes > 0 ? cfg_.zone_size_bytes : cfg_.segment_bytes;

    // With striping: segment holds stripe_width zones worth of blocks
    segment_size_blocks = (zone_capacity / blk_sz) * stripe_width;

    // Calculate total zones first, then divide by stripe_width
    std::size_t total_zones = cache_block_count * blk_sz / zone_size;
    total_segments = total_zones / stripe_width;

    total_cache_block_count = cache_block_count;
    total_capacity_bytes = cache_block_count * blk_sz;
    assert(segment_size_blocks > 0 && "segment_bytes too small");
    assert(total_segments      > 0 && "device_bytes too small");
    log_cache_timestamp = 0;

    SPDK_NOTICELOG("LogCache init: stripe_width=%d, zone_capacity=%zuMB, zone_size=%zuMB\n",
           stripe_width, zone_capacity / (1024*1024), zone_size / (1024*1024));
    SPDK_NOTICELOG("LogCache init: segment_size_blocks=%zu, total_zones=%zu, total_segments=%zu\n",
           segment_size_blocks, total_zones, total_segments);

    // Re-initialize EWMA ratios with segment_size_blocks (instead of hardcoded DEFAULT_HALF_LIFE_IN_BLOCKS)
    compaction_ratio = EwmaRatio::FromHalfLifeBlocks(segment_size_blocks * 4);
    eviction_ratio = EwmaRatio::FromHalfLifeBlocks(segment_size_blocks * 4);
    eviction_ratio_in_ghost_cache = EwmaRatio::FromHalfLifeBlocks(segment_size_blocks * 4);

    evictor->init(&log_cache_timestamp, segment_size_blocks, total_segments);

    if (compactor) {
        compactor->init(&log_cache_timestamp, segment_size_blocks, total_segments);
    }

    stream_policy = input_stream_policy;
    global_valid_blocks = 0;

    /* 세그먼트 전부 미리 생성 → free_pool */
    /* With striping: each segment has stripe_width physical_bases */
    for (std::size_t i = 0; i < total_segments; ++i)
    {
        auto seg = std::make_unique<LogCacheSegment>(segment_size_blocks, log_cache_timestamp, stripe_width);
        seg->zone_capacity_bytes_ = zone_capacity;
        // Set physical_bases for each zone in the stripe
        for (int z = 0; z < stripe_width; ++z) {
            std::size_t zone_idx = i * stripe_width + z;
            seg->physical_bases[z] = zone_idx * zone_size;
        }
        // Legacy compatibility
        seg->physical_base = seg->physical_bases[0];
        free_pool.push_back(seg.get());
        all_segments.push_back(std::move(seg));
    }

    if (cache_trace_)
    {
        if (!trace_file.empty())
            trace_fp_ = fopen(trace_file.c_str(), "w");
        if (!cold_trace.empty())
            cold_trace_fp_ = fopen(cold_trace.c_str(), "w");
    }
}

LogCache::~LogCache()
{
    if (trace_fp_)      std::fclose(trace_fp_);
    if (cold_trace_fp_) std::fclose(cold_trace_fp_);
}

uint64_t LogCache::block_offset(const LogCacheSegment *seg, std::size_t idx) const
{
    // Use striped offset calculation
    return seg->get_block_offset(idx, cache_block_size);
}

int LogCache::write_cache_data(uint64_t offset, const void *buf, size_t len)
{
    if (!device_io_ || buf == nullptr || len == 0) {
        return 0;
    }
    return device_io_->write_cache(offset, buf, len);
}

int LogCache::read_cache_data(uint64_t offset, void *buf, size_t len)
{
    if (!device_io_ || buf == nullptr || len == 0) {
        return 0;
    }
    return device_io_->read_cache(offset, buf, len);
}

int LogCache::write_backend_data(uint64_t offset, const void *buf, size_t len)
{
    if (!device_io_ || buf == nullptr || len == 0) {
        return 0;
    }
    return device_io_->write_backend(offset, buf, len);
}

int LogCache::read_backend_data(uint64_t offset, void *buf, size_t len)
{
    if (!device_io_ || buf == nullptr || len == 0) {
        return 0;
    }
    return device_io_->read_backend(offset, buf, len);
}

void LogCache::reset_cache_region(LogCacheSegment *seg)
{
    if (!device_io_ || seg == nullptr) {
        return;
    }
    // For striped segments: reset all N zones
    std::size_t zone_size = cfg_.zone_size_bytes > 0 ? cfg_.zone_size_bytes : cfg_.segment_bytes;
    std::size_t reset_len = zone_size * seg->stripe_width_;
    device_io_->reset_cache_region(seg->physical_bases[0], reset_len);
}

void LogCache::ensure_staging_buffer(size_t len)
{
    if (staging_buffer_.size() < len) {
        staging_buffer_.resize(len);
    }
}

void LogCache::copy_block(LogCacheSegment *src_seg,
                          std::size_t src_idx,
                          LogCacheSegment *target_seg)
{
    if (!target_seg) {
        return;
    }
    auto &src_blk = src_seg->blocks[src_idx];
    auto &dst_blk = target_seg->blocks[target_seg->write_ptr];
    if (device_io_) {
        ensure_staging_buffer(cache_block_size);
        read_cache_data(block_offset(src_seg, src_idx),
                        staging_buffer_.data(),
                        cache_block_size);
        write_cache_data(block_offset(target_seg, target_seg->write_ptr),
                         staging_buffer_.data(),
                         cache_block_size);
    }
    dst_blk.key = src_blk.key;
    dst_blk.valid = true;
    dst_blk.create_timestamp = src_blk.create_timestamp;
    mapping[src_blk.key] = { target_seg, target_seg->write_ptr };
    ++target_seg->write_ptr;
    ++target_seg->valid_cnt;
    ++compacted_blocks;
    src_blk.valid = false;
}

void LogCache::flush_block_to_backend(LogCacheSegment *seg, std::size_t idx)
{
    if (!device_io_) {
        return;
    }
    auto &blk = seg->blocks[idx];
    if (!blk.valid) {
        return;
    }
    ensure_staging_buffer(cache_block_size);
    read_cache_data(block_offset(seg, idx), staging_buffer_.data(), cache_block_size);
    uint64_t backend_offset = static_cast<uint64_t>(blk.key) * cache_block_size;
    write_backend_data(backend_offset, staging_buffer_.data(), cache_block_size);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
bool LogCache::exists(long key)
{
    return mapping.find(key) != mapping.end();
}


void LogCache::invalidate(long key, int lba_sz) {
    if (exists(key))
    {
        auto loc = mapping[key];
        if (loc.seg->blocks[loc.idx].valid)
        {
            print_objects("invalidate", log_cache_timestamp - loc.seg->blocks[loc.idx].create_timestamp);
            invalidate_blocks += 1;
            write_hit_size += 1;  // Write cache hit: same LBA exists, invalidating old block
            loc.seg->blocks[loc.idx].valid = false;
            --loc.seg->valid_cnt;
            global_valid_blocks -= 1;
            if (loc.seg->full()){
                evict_policy_update(loc.seg);
            }
        }
       // else{
        //    assert(false);
       // }
        mapping.erase(key);
    }
    else
    {
        if (evicted_timestamp.find(key) != evicted_timestamp.end()) {
            reinsert_blocks++;
            print_objects("reinsert", log_cache_timestamp - evicted_timestamp[key]);
            evicted_timestamp.erase(key);
        }
        if (device_io_) {
            device_io_->trim_backend(static_cast<uint64_t>(key) * cache_block_size, lba_sz);
        } else {
            _invalidate_cold_block(key * cache_block_size,
                                    lba_sz,
                                    OP_TYPE::TRIM);
        }
    }
}

void LogCache::evict_policy_add(LogCacheSegment *s) {
    evictor->add(s, log_cache_timestamp);
    if (compactor) {
        compactor->add(s, log_cache_timestamp);
    }
}

void LogCache::evict_policy_remove(LogCacheSegment *s) {
    evictor->remove(s);
    if (compactor) {
        compactor->remove(s);
    }
}

void LogCache::evict_policy_update(LogCacheSegment *s) {
    evictor->update(s);
    if (compactor) {
        compactor->update(s);
    }
}

void LogCache::periodic() {
    if (is_ghost_cache){
        if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
            compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
            eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
            uint64_t evicted_in_ghost = ghost_cache.evictCount();
            eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
            compaction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, ghost_compacted_blocks);
        }
        if (log_cache_timestamp % (segment_size_blocks * 4) == 0) {
            if (compaction_ratio.has_value() &&
                eviction_ratio.has_value() &&
                eviction_ratio_in_ghost_cache.has_value() &&
                compaction_ratio_in_ghost_cache.has_value()){

                double eviction_delta = eviction_ratio.value() - eviction_ratio_in_ghost_cache.value();
                double compaction_delta = compaction_ratio_in_ghost_cache.value() - compaction_ratio.value();

                // eviction_ratio > 1.3이면 무조건 LOWER (full random workload → eviction-heavy)
                if (compaction_ratio.value() > 1.3) {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - 0.04);
                    SPDK_NOTICELOG("periodic: LOWER (evict>30%%) target=%.4f, evict_delta=%.6f, compact_delta=%.6f, m=%d\n",
                                   target_valid_blk_rate, 6.73 * eviction_delta,
                                   compaction_delta, last_ghost_m);
                }
                else if (6.73 * eviction_delta > compaction_delta) {
                    target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, (double) global_valid_blocks / total_cache_block_count + 0.04);
                    SPDK_NOTICELOG("periodic: RISE target=%.4f, evict_delta=%.6f, compact_delta=%.6f, m=%d\n",
                                   target_valid_blk_rate, 6.73 * eviction_delta,
                                   compaction_delta, last_ghost_m);
                }
                else {
                    target_valid_blk_rate = std::max(0.0, (double)global_valid_blocks / total_cache_block_count - 0.02);
                    SPDK_NOTICELOG("periodic: LOWER target=%.4f, evict_delta=%.6f, compact_delta=%.6f, m=%d\n",
                                   target_valid_blk_rate, 6.73 * eviction_delta,
                                   compaction_delta, last_ghost_m);
                }
            }
        }
    }
}

void LogCache::batch_insert(int stream_id,
                            const std::map<long,int>& newBlocks,
                            OP_TYPE                   op_type)
{
    if (op_type == OP_TYPE::READ || newBlocks.empty())
        return;                          // 요구사항 ③ – read 무시
    
    for (std::map<long, int>::const_iterator it = newBlocks.begin();
         it != newBlocks.end(); ++it) {
        append_block(stream_id, it->first, it->second, nullptr);
    }
    check_and_evict_if_needed();   
    /* 3) free pool 부족 시 세그먼트 eviction */
    
}

void LogCache::append_block(int stream_id, long key, int lba_sz, const void *payload)
{
    periodic();
    ghost_cache.access(key);
    LogCacheSegment* seg = nullptr;
    if (stream_policy) {
        seg = get_segment_with_stream_policy(false, key);
    } else {
        seg = get_segment_to_active_stream(false, stream_id);
    }

    if (seg->full()) {
        evict_policy_add(seg);
        int assigned_class_num = seg->get_class_num();
        active_seg.erase(seg->get_class_num());
        seg = get_segment_to_active_stream(false, assigned_class_num);
    }

    invalidate(key, lba_sz);

    auto &blk = seg->blocks[seg->write_ptr];
    uint64_t dst_offset = block_offset(seg, seg->write_ptr);
    blk.key = key;
    blk.valid = true;
    blk.create_timestamp = log_cache_timestamp;
    mapping[key] = { seg, seg->write_ptr };

    ++seg->write_ptr;
    ++seg->valid_cnt;
    ++global_valid_blocks;
    ++log_cache_timestamp;
    if (stream_policy) {
        stream_policy->Append(key, log_cache_timestamp, reinterpret_cast<void*>(seg->valid_cnt));
    }
    write_size_to_cache += lba_sz;

    if (payload) {
        write_cache_data(dst_offset, payload, lba_sz);
    }
}

bool LogCache::append_block_metadata(int stream_id, long key, int lba_sz, uint64_t *cache_offset, int *out_stream_id, bool *out_segment_full)
{
    periodic();
    ghost_cache.access(key);
    LogCacheSegment* seg = nullptr;
    if (stream_policy) {
        seg = get_segment_with_stream_policy(false, key);
    } else {
        seg = get_segment_to_active_stream(false, stream_id);
    }

    if (!seg) {
        return false;
    }

    if (seg->full()) {
        evict_policy_add(seg);
        int assigned_class_num = seg->get_class_num();
        active_seg.erase(seg->get_class_num());
        // Use assigned_class_num to maintain stream policy classification
        // host_write_handle_ (0,1) is separate from class_num (stream policy)
        seg = get_segment_to_active_stream(false, assigned_class_num);
        if (!seg) {
            return false;
        }
    }

    invalidate(key, lba_sz);

    // Verify class_num is valid (should be set by get_segment_to_active_stream)
    if (seg->get_class_num() < 0) {
        SPDK_ERRLOG("BUG! append_block: seg=%p has invalid class_num=%d (uninitialized?)\n",
                (void*)seg, seg->get_class_num());
    }

    auto &blk = seg->blocks[seg->write_ptr];
    uint64_t dst_offset = block_offset(seg, seg->write_ptr);
    blk.key = key;
    blk.valid = true;
    blk.create_timestamp = log_cache_timestamp;
    mapping[key] = { seg, seg->write_ptr };
    pending_writes_.insert(key);  // Mark as write in progress

    ++seg->write_ptr;
    ++seg->valid_cnt;
    ++global_valid_blocks;
    ++log_cache_timestamp;
    if (stream_policy) {
        stream_policy->Append(key, log_cache_timestamp, reinterpret_cast<void*>(seg->valid_cnt));
    }
    
    write_size_to_cache += lba_sz;

    *cache_offset = dst_offset;
    // Return the current host_write_handle for FDP placement
    // (this block will be written with this handle)
    if (out_stream_id) {
        *out_stream_id = host_write_handle_;
    }

    // Check if segment became full after this write - add to evictor immediately
    // This prevents full segments from staying in active_seg when next write goes to different stream
    bool segment_full = seg->full();
    if (segment_full) {
        evict_policy_add(seg);
        active_seg.erase(seg->get_class_num());
        // Toggle for next block (after segment full)
        toggle_host_write_handle();
    }
    if (out_segment_full) {
        *out_segment_full = segment_full;
    }

    return true;
}

void LogCache::complete_block_write(long key)
{
    pending_writes_.erase(key);
}

void LogCache::complete_block_writes(const std::vector<long>& keys)
{
    for (long key : keys) {
        pending_writes_.erase(key);
    }
}

void LogCache::ingest_payload(int stream_id, const std::vector<BlockPayload>& payloads)
{
    if (payloads.empty()) {
        return;
    }
    for (const auto &entry : payloads) {
        append_block(stream_id, entry.key, entry.lba_size, entry.data);
    }
    check_and_evict_if_needed();
}

int LogCache::read_block(long key, void *buf, size_t len)
{
    if (!buf || len == 0) {
        return -1;
    }
    auto it = mapping.find(key);
    if (it != mapping.end()) {
        auto &blk = it->second.seg->blocks[it->second.idx];
        if (!blk.valid) {
            return -1;
        }
        return read_cache_data(block_offset(it->second.seg, it->second.idx), buf, len);
    }
    return read_backend_data(static_cast<uint64_t>(key) * cache_block_size, buf, len);
}

int LogCache::read_blocks(const std::vector<BlockReadRequest>& reqs)
{
    for (const auto &req : reqs) {
        int rc = read_block(req.key, req.data, req.len);
        if (rc) {
            return rc;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
LogCacheSegment* LogCache::alloc_segment(bool shrink)
{
    // In async mode, keep segments reserved for GC/Evict to have room to work
    // shrink=true means host write, shrink=false means GC
    // Only block host writes when low on segments, GC must always be able to allocate
#if FDP
    constexpr size_t ASYNC_RESERVE_SEGMENTS = CRITICAL_FREE_SEGMENTS;  // FDP: smaller segments, need more reserve
#else
    constexpr size_t ASYNC_RESERVE_SEGMENTS = CRITICAL_FREE_SEGMENTS;
#endif
    if (async_mode_ && shrink && free_pool.size() <= ASYNC_RESERVE_SEGMENTS) {
        return nullptr;  // Host write blocked - caller should trigger async GC/Evict
    }

    if (free_pool.empty()) {
        throw std::runtime_error("LogCache: no free segment");
    }

    LogCacheSegment* s = free_pool.front();
    free_pool.pop_front();

    // DEBUG: check class_num BEFORE reset
    int class_num_before = s->get_class_num();

    s->reset();

    // DEBUG: check class_num AFTER reset - should be -1 now
    if (s->get_class_num() != -1) {
        SPDK_ERRLOG("BUG! alloc_segment: after reset(), class_num=%d (expected -1), before=%d\n",
                s->get_class_num(), class_num_before);
    }

    // DEBUG: check if segment is already in use (active_seg or gc_active_seg)
    for (auto &kv : active_seg) {
        if (kv.second == s) {
            SPDK_ERRLOG("BUG! alloc_segment: seg=%p ALREADY IN active_seg[%d]!\n",
                    (void*)s, kv.first);
        }
    }
    for (auto &kv : gc_active_seg) {
        if (kv.second == s) {
            SPDK_ERRLOG("BUG! alloc_segment: seg=%p ALREADY IN gc_active_seg[%d]!\n",
                    (void*)s, kv.first);
        }
    }

    // DEBUG: log segment allocation with class_num
    SPDK_NOTICELOG("DEBUG alloc_segment: seg=%p, class_num=%d, shrink=%d, free_pool=%zu\n",
            (void*)s, s->get_class_num(), shrink, free_pool.size());

    // Log when free segments drop to critical level
    size_t remaining = free_pool.size();
    if (remaining <= ASYNC_RESERVE_SEGMENTS) {
        SPDK_WARNLOG("LOW FREE SEGMENTS: %zu remaining (shrink=%d, async=%d)\n",
                     remaining, shrink, async_mode_);
    }

    return s;
}


LogCacheSegment* LogCache::get_segment_with_stream_policy(bool gc, uint64_t key, bool check_only)
{
    LogCacheSegment *seg = nullptr;
    uint64_t previous_blk_create_timestamp = log_cache_timestamp;  // Default to current timestamp for new keys
    if (gc && exists(key))
    {
        auto loc = mapping[key];
        assert(loc.seg != nullptr);
        assert(loc.idx < loc.seg->blocks.size());
        if (loc.seg->blocks[loc.idx].valid)
        {
            previous_blk_create_timestamp = loc.seg->blocks[loc.idx].create_timestamp;
        }
    }
    int stream_id = stream_policy->Classify(key, gc, log_cache_timestamp, previous_blk_create_timestamp);
    assert (!gc || (gc && stream_id >= Segment::GC_STREAM_START));
    std::unordered_map<int, LogCacheSegment*>*  active_table = &active_seg;
    if (gc) {
        active_table = &gc_active_seg;
    }
    auto it_stream = active_table->find(stream_id);
    if (it_stream == active_table->end())
    {
        if(check_only) {
            return nullptr;
        }
        seg = alloc_segment(!gc);
        if (!seg) {
            return nullptr;  // No free segment (async mode needs GC/Evict)
        }
        int old_class_num = seg->get_class_num();
        seg->class_num = stream_id; // stream id로 class num 설정
        seg->create_timestamp = log_cache_timestamp; // 초기화
        (*active_table)[stream_id] = seg;
        // DEBUG: always log class_num assignment
        SPDK_NOTICELOG("DEBUG get_segment_with_stream_policy: seg=%p, old_class=%d, new_class=%d, stream_id=%d\n",
                (void*)seg, old_class_num, seg->get_class_num(), stream_id);
        // DEBUG: verify class_num was set correctly
        if (seg->get_class_num() != stream_id) {
            SPDK_ERRLOG("BUG! get_segment_with_stream_policy: class_num=%d after setting to %d, old=%d\n",
                    seg->get_class_num(), stream_id, old_class_num);
        }
    }
    else
    {
        seg = it_stream->second;
        assert (!gc || (gc && stream_id >= Segment::GC_STREAM_START));
        assert (!gc || (gc && seg->get_class_num() >= Segment::GC_STREAM_START));
    }
    return seg;
}

LogCacheSegment* LogCache::get_segment_to_active_stream(bool gc, int stream_id, bool check_only)
{
    LogCacheSegment *seg = nullptr;
    std::unordered_map<int, LogCacheSegment*>*  active_table = &active_seg;
    if (gc) {
        active_table = &gc_active_seg;
        if (stream_id < Segment::GC_STREAM_START) {
            stream_id += Segment::GC_STREAM_START; 
        }
    }
    auto it_stream = active_table->find(stream_id);
    if (it_stream == active_table->end())
    {
        if (check_only) {
            return nullptr;
        }
        seg = alloc_segment(!gc);
        if (!seg) {
            return nullptr;  // No free segment (async mode needs GC/Evict)
        }
        int old_class_num = seg->get_class_num();
        seg->class_num = stream_id; // stream id로 class num 설정
        seg->create_timestamp = log_cache_timestamp; // 초기화
        (*active_table)[stream_id] = seg;
        // DEBUG: verify class_num was set correctly
        if (seg->get_class_num() != stream_id) {
            SPDK_ERRLOG("BUG! get_segment_to_active_stream: class_num=%d after setting to %d, old=%d\n",
                    seg->get_class_num(), stream_id, old_class_num);
        }
    }
    else
    {
        seg = it_stream->second;
    }
    return seg;
}

extern uint64_t g_threshold;
extern uint64_t g_timestamp;
void LogCache::check_and_evict_if_needed()
{
    // In async mode, GC/Evict is handled by async wrapper
    // Sync eviction here would cause re-entrancy issues with SPDK pollers
    if (async_mode_) {
        return;
    }

    const std::size_t low_water =
        static_cast<std::size_t>(std::ceil(total_segments *
                                           cfg_.free_ratio_low));
    LogCacheSegment* last_target_seg = nullptr;
    //bool first_compact = true;
    std::list<Segment *> segment_list;
    g_timestamp = log_cache_timestamp;
    static uint64_t threshold = (cfg_.segment_bytes / cache_block_size) *static_cast<std::size_t>(std::ceil(total_segments *
                                           (1 - cfg_.free_ratio_low) * (1 + additional_free_blks_ratio_by_gc)));
    g_threshold = threshold;
    while (free_pool.size() < low_water)
    {
        /* eviction 후보 수집 */
        bool compact = false;
        if (target_valid_blk_rate >= 0.1) {
            if (compactor && (double)target_valid_blk_rate * total_cache_block_count  > global_valid_blocks) {
                compact = true;
            }
        }

        LogCacheSegment* victim = nullptr; 
        if (compact == true){
            victim = (LogCacheSegment *)evictor->choose_segment();
            threshold = log_cache_timestamp - victim->create_timestamp + 1;
            g_threshold = threshold; 
            
            if (additional_free_blks_ratio_by_gc < 0.01 ||
                log_cache_timestamp - victim->create_timestamp < threshold) {
                evictor->add(victim, log_cache_timestamp);
                victim = (LogCacheSegment *)compactor->choose_segment();
            }
            else {
                // addtional_free_blks_ratio_by_gc is on 
                // and threshold < log_cache_timestamp - victim->create_timestamp
                // exception path
                compact = false; 
            }
        }
        else {
            victim = (LogCacheSegment *)evictor->choose_segment();
            threshold = log_cache_timestamp - victim->create_timestamp;
            g_threshold = threshold;    
        }

        //printf("compact %d\n", compact);
        assert (victim != nullptr);
        if (victim->valid_cnt == 0) {
            reset_segment(victim);
        }
        else if (compact == true) {
            int stream_id = victim->get_class_num();
            last_target_seg = (LogCacheSegment *)evict_and_compaction(victim, threshold, stream_id);
            if (last_target_seg) {
                segment_list.push_back(last_target_seg);
            }
        }
        else {
            evicted_segment_age = victim->get_create_time();
            evict_segment(victim);
        }

        if (stream_policy && compact == true) {
            stream_policy->CollectSegment(victim, log_cache_timestamp);
        }
        if (stream_policy) {
            int victim_stream_id = stream_policy->GetVictimStreamId(log_cache_timestamp, threshold);
            while (stream_policy && victim_stream_id >= Segment::GC_STREAM_START) {
                LogCacheSegment* seg = get_segment_to_active_stream(true, victim_stream_id, true);
                if (seg){
                   // printf("Evict GC stream %d, write_ptr %d\n", victim_stream_id, seg->write_ptr);
                    dummy_fill_segment(seg);
                    //evict_segment(seg);
                    gc_active_seg.erase(victim_stream_id);
                }
                else {
                    break;
                }
            }
        }
    }
}

int LogCache::get_block_size()
{
    return cache_block_size;
}

bool LogCache::is_cache_filled() {
    const std::size_t low_water =
        static_cast<std::size_t>(std::ceil(total_segments *
                                           cfg_.free_ratio_low));

    if (async_mode_ && free_pool.size() <= LOW_FREE_SEGMENTS) {
        return true;
    }

    return free_pool.size() < low_water;
}


void LogCache::reset_segment(LogCacheSegment* s)
{
    // DEBUG: log segment reset
    SPDK_NOTICELOG("DEBUG reset_segment: seg=%p, class_num=%d, old_write_ptr=%zu, "
            "old_valid_cnt=%ld, create_ts=%lu\n",
            (void*)s, s->get_class_num(), s->write_ptr, s->valid_cnt, s->create_timestamp);

    // erase old segment
    s->valid_cnt = 0;
    s->write_ptr = 0;
    reset_cache_region(s);

    // DEBUG: check for duplicate in free_pool before adding
    for (auto *seg : free_pool) {
        if (seg == s) {
            SPDK_ERRLOG("BUG! reset_segment: seg=%p ALREADY IN free_pool! "
                    "free_pool_size=%zu\n", (void*)s, free_pool.size());
        }
    }

    free_pool.push_back(s);
    evict_policy_remove(s);
}

// Async context for reset_segment_async
struct ResetSegmentAsyncCtx {
    LogCache *cache;
    LogCacheSegment *seg;
    cache_device_io_cb user_cb;
    void *user_cb_arg;
};

static void reset_segment_async_cb(void *cb_arg, int status) {
    auto *ctx = static_cast<ResetSegmentAsyncCtx *>(cb_arg);
    // Add segment to free pool after zone reset completes
    ctx->cache->complete_segment_reset(ctx->seg);
    if (ctx->user_cb) {
        ctx->user_cb(ctx->user_cb_arg, status);
    }
    delete ctx;
}

void LogCache::reset_segment_async(LogCacheSegment* s, cache_device_io_cb cb, void *cb_arg)
{
    if (!s) {
        if (cb) cb(cb_arg, 0);
        return;
    }

    // Clear segment state first
    s->valid_cnt = 0;
    s->write_ptr = 0;
    evict_policy_remove(s);

    if (!device_io_) {
        // No device, just add to free pool directly
        // DEBUG: check for duplicate
        for (auto *seg : free_pool) {
            if (seg == s) {
                SPDK_ERRLOG("BUG! reset_segment_async(no device): seg=%p ALREADY IN free_pool!\n", (void*)s);
            }
        }
        free_pool.push_back(s);
        if (cb) cb(cb_arg, 0);
        return;
    }

    // Create async context
    auto *ctx = new ResetSegmentAsyncCtx{this, s, cb, cb_arg};
    // For striped segments: reset all N zones
    // Use zone_size * stripe_width to cover all zones (not zone_capacity)
    std::size_t zone_size = cfg_.zone_size_bytes > 0 ? cfg_.zone_size_bytes : cfg_.segment_bytes;
    std::size_t reset_len = zone_size * s->stripe_width_;
    SPDK_NOTICELOG("reset_segment_async: seg=%p, physical_bases[0]=0x%lx, zone_size=%zu, stripe_width=%d, reset_len=%zu\n",
           s, s->physical_bases[0], zone_size, s->stripe_width_, reset_len);
    device_io_->reset_cache_region_async(s->physical_bases[0], reset_len,
                                          reset_segment_async_cb, ctx);
}

void LogCache::complete_segment_reset(LogCacheSegment* s)
{
    if (s) {
        // DEBUG: check for duplicate
        for (auto *seg : free_pool) {
            if (seg == s) {
                SPDK_ERRLOG("BUG! complete_segment_reset: seg=%p ALREADY IN free_pool!\n", (void*)s);
            }
        }
        free_pool.push_back(s);
        SPDK_NOTICELOG("DEBUG complete_segment_reset: seg=%p, free_pool=%zu\n",
                (void*)s, free_pool.size());
    }
}

void LogCache::dummy_fill_segment(LogCacheSegment* s)
{
    if (s) {
        for (std::size_t i = s->write_ptr; i < s->blocks.size(); ++i) {
            s->blocks[i].key = 0;
            s->blocks[i].valid = false;
            s->blocks[i].create_timestamp = UINT64_MAX;
        }
        s->write_ptr = s->blocks.size();
        evict_policy_add(s);
    }
}



Segment* LogCache::evict_and_compaction(LogCacheSegment* s, uint64_t threshold, int gc_stream_id)
{
    LogCacheSegment* target_seg = nullptr;
    int evicted_blocks_for_victim = 0, compacted_blocks_for_victim = 0;
    if (!stream_policy) {
        target_seg = get_segment_to_active_stream(true, gc_stream_id);
    }
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) continue;
        if (threshold > 0 && log_cache_timestamp - blk.create_timestamp >= threshold) {
            if (is_ghost_cache) {
                ghost_cache.push(blk.key);
            }
            print_objects("evict", log_cache_timestamp - blk.create_timestamp);
            evicted_blocks += cfg_.evicted_blk_size;
            evicted_timestamp[blk.key] = log_cache_timestamp;
            evicted_blocks_for_victim += 1;
            // map erase and blk valid false is done in this function
            evict(s, i);
            continue;
        }
        if (stream_policy) {
            target_seg = get_segment_with_stream_policy(true, blk.key);
        }
        assert(target_seg->class_num >= Segment::GC_STREAM_START || !stream_policy);
        if (target_seg->full())                 // segment 소진 -> 새 seg
        {
            // add previous segment count 
            evict_policy_add(target_seg);
            int assigned_class_num = target_seg->get_class_num();
            assert(assigned_class_num >= 0);
            gc_active_seg.erase(target_seg->get_class_num());
            target_seg = get_segment_to_active_stream(true, assigned_class_num);
        }
        assert(target_seg != s);
        // get p2L index
        if (target_seg->create_timestamp > blk.create_timestamp) {
            target_seg->create_timestamp = blk.create_timestamp;
        }

        print_objects("compact", log_cache_timestamp - blk.create_timestamp);
        copy_block(s, i, target_seg);
        compacted_blocks_for_victim += 1;

    }
    assert((target_seg == nullptr && compacted_blocks_for_victim == 0) || target_seg);
    /*if (target_seg) {
        printf("Compaction and Evict: %lu blocks moved from segment %p to segment %p, free_pool_size %ld, valid ratio %.4f age %lu target_seg_write_ptr %lu target_create_timestamp %lu threshold %lu stream_id %d\n", 
        s->valid_cnt, s, target_seg, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, target_seg->write_ptr, s->create_timestamp, threshold, target_seg->get_class_num());
    }
    else {
        printf("Evict: %lu blocks free_pool_size %ld, valid ratio %.4f age %lu, create_time %lu \n",
        s->valid_cnt, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, s->create_timestamp);
    }*/
    reset_segment(s);
    return target_seg;
}


void LogCache::evict_segment(LogCacheSegment* s)
{
    int evicted_blocks_for_victim = 0;
    /* 모든 valid page flush */
    if (s->valid_cnt == 0) {
        SPDK_NOTICELOG("valid_cnt=%ld\n", s->valid_cnt);
        SPDK_NOTICELOG("No valid blocks to evict.\n");
    }
    
    for (std::size_t i = 0; i < s->blocks.size(); ++i)
    {
        auto &blk = s->blocks[i];
        if (!blk.valid) {
            continue;
        }
        if (is_ghost_cache) {
            ghost_cache.push(blk.key);
        }
        print_objects("evict", log_cache_timestamp - blk.create_timestamp);
        evicted_blocks += cfg_.evicted_blk_size;
        evicted_blocks_for_victim += 1;
        evicted_timestamp[blk.key] = log_cache_timestamp;

        evict(s, i);

    }
  //  printf("Evict: %lu blocks free_pool_size %ld, valid ratio %.4f age %lu, create_time %lu \n",
  //      s->valid_cnt, free_pool.size(), global_valid_blocks / (float)total_cache_block_count, log_cache_timestamp - s->create_timestamp, s->create_timestamp);
    reset_segment(s);
}


void LogCache::evict_one_block() {
}

void LogCache::evict(LogCacheSegment *seg, std::size_t idx) {
    
    //const uint64_t DUMMY_VALUE = 0;
    auto &blk = seg->blocks[idx];
    uint64_t old_key = blk.key;
    int EVICTED_BLOCK_SIZE = cfg_.evicted_blk_size; // 16 blocks, 64k
    int evicted_blocks_per_evict = 0;
    struct ChunkBlock {
        LogCacheSegment* seg;
        std::size_t idx;
        bool valid;
        ChunkBlock() : seg(nullptr), idx(0), valid(false) {}
        ChunkBlock(LogCacheSegment* s, std::size_t i, bool v)
            : seg(s), idx(i), valid(v) {}
    };
    std::vector<ChunkBlock> chunk_blocks;
    chunk_blocks.reserve(EVICTED_BLOCK_SIZE);

    uint64_t start_index_64k = old_key / EVICTED_BLOCK_SIZE * EVICTED_BLOCK_SIZE;
    for (uint64_t index_64k = start_index_64k; index_64k < start_index_64k + EVICTED_BLOCK_SIZE; index_64k++) {
        if (index_64k == old_key) {
            chunk_blocks.push_back(ChunkBlock(seg, idx, true));
            continue;
        }
        auto it = mapping.find(index_64k);

        if (it != mapping.end()) {
            auto loc = it->second;
            chunk_blocks.push_back(ChunkBlock(loc.seg, loc.idx, true));
            mapping.erase(index_64k);
            auto &other_blk = loc.seg->blocks[loc.idx];
            other_blk.valid = false;
            global_valid_blocks -= 1;
            evicted_blocks_per_evict += 1;
        }
        else {
            chunk_blocks.push_back(ChunkBlock(nullptr, 0, false));
            read_blocks_in_partial_write += 1;
        }
    }

    mapping.erase(blk.key);
    blk.valid = false;
    global_valid_blocks -= 1;

    if (device_io_) {
        ensure_staging_buffer(cache_block_size * EVICTED_BLOCK_SIZE);
        for (int i = 0; i < EVICTED_BLOCK_SIZE; ++i) {
            uint8_t *dst = staging_buffer_.data() + static_cast<size_t>(i) * cache_block_size;
            auto &block_ref = chunk_blocks[i];
            if (block_ref.valid) {
                read_cache_data(block_offset(block_ref.seg, block_ref.idx), dst, cache_block_size);
            } else {
                read_backend_data((start_index_64k + i) * cache_block_size, dst, cache_block_size);
            }
        }
        write_backend_data(start_index_64k * cache_block_size,
                           staging_buffer_.data(),
                           static_cast<size_t>(cache_block_size) * EVICTED_BLOCK_SIZE);
    } else {
        _evict_one_block(start_index_64k  * cache_block_size /* 64k aligend */, cache_block_size * EVICTED_BLOCK_SIZE /* 64k */, OP_TYPE::WRITE);
    }

    evicted_cache_blocks_per_evict->inc(evicted_blocks_per_evict);
}

void LogCache::print_objects(std::string prefix, uint64_t value) {
    //fprintf(fp_object, "%s: %lu\n", prefix.c_str(), value);
}

void LogCache::print_stats() {
    static uint64_t written_window_bytes = cfg_.print_stats_interval;
    static uint64_t next_written_bytes = cfg_.segment_bytes;
    if ((uint64_t)write_size_to_cache >= next_written_bytes) {
        const std::string& prefix = stats_prefix();
        const char* prefix_cstr = prefix.empty() ? "LOG_CACHE" : prefix.c_str();
        fprintf (fp_stats, "%s invalidate_blocks: %lu compacted_blocks: %lu global_valid_blocks: %lu write_size_to_cache: %llu evicted_blocks: %llu write_hit_size: %llu total_cache_size: %lu reinsert_blocks: %lu read_blocks_in_partial_write %lu\n",
                prefix_cstr, invalidate_blocks, compacted_blocks, global_valid_blocks, write_size_to_cache, evicted_blocks, write_hit_size, total_capacity_bytes, reinsert_blocks, read_blocks_in_partial_write);
        fflush(fp_stats);
        next_written_bytes += written_window_bytes;
    }
}

void LogCache::print_histograms(bool reset) {
    if (evicted_ages_histogram) evicted_ages_histogram->print_current(reset);
    if (evicted_blocks_histogram) evicted_blocks_histogram->print_current(reset);
    if (compacted_blocks_histogram) compacted_blocks_histogram->print_current(reset);
    if (evicted_ages_with_segment_histogram) evicted_ages_with_segment_histogram->print_current(reset);
    if (compacted_ages_with_segment_histogram) compacted_ages_with_segment_histogram->print_current(reset);
    if (evicted_cache_blocks_per_evict) evicted_cache_blocks_per_evict->print_current(reset);
}

/* ------------------------------------------------------------------ */
/* Async GC/Evict helpers                                              */
/* ------------------------------------------------------------------ */
bool LogCache::need_gc_or_evict() const
{
    const std::size_t low_water =
        static_cast<std::size_t>(std::ceil(total_segments * cfg_.free_ratio_low));

    // Debug: periodically print free_pool size
    static uint64_t check_count = 0;
    if (++check_count % 100000 == 0) {
        SPDK_NOTICELOG("GC check: free_pool=%zu, total_segments=%zu, low_water=%zu\n",
               free_pool.size(), total_segments, low_water);
    }

    // In async mode, trigger GC early (before blocking host IO)
    // GC_TRIGGER: start GC while host IO continues
    // BLOCK threshold (in get_free_segment): stop host IO

    if (async_mode_ && free_pool.size() <= LOW_FREE_SEGMENTS) {
        // Don't trigger GC if evictor is empty (nothing to evict)
        if (evictor->empty()) {
            return false;
        }
        static uint64_t gc_trigger_count = 0;
        if (++gc_trigger_count % 1000 == 1) {
            SPDK_NOTICELOG("GC triggered! free_pool=%zu <= %zu\n", free_pool.size(), LOW_FREE_SEGMENTS);
        }
        return true;
    }

    return free_pool.size() < low_water;
}

bool LogCache::get_cache_location(long key, uint64_t *offset)
{
    auto it = mapping.find(key);
    if (it == mapping.end()) {
        return false;  // Not in cache - caller should try backend
    }

    LogCacheSegment *seg = it->second.seg;
    size_t idx = it->second.idx;

    // Validation: check segment and index are valid
    if (!seg) {
        SPDK_ERRLOG("get_cache_location: key=%ld has NULL segment!\n", key);
        return false;
    }
    if (idx >= seg->blocks.size()) {
        SPDK_ERRLOG("get_cache_location: key=%ld idx=%zu >= blocks.size=%zu!\n",
                key, idx, seg->blocks.size());
        return false;
    }
    if (!seg->blocks[idx].valid) {
        SPDK_ERRLOG("get_cache_location: key=%ld MAPPING BUT VALID=FALSE! "
                "seg=%p, idx=%zu, block.key=%ld, seg->create_ts=%lu\n",
                key, (void*)seg, idx, seg->blocks[idx].key, seg->create_timestamp);
        return false;
    }
    // Validation: check that the block's key matches our lookup key
    if (seg->blocks[idx].key != key) {
        SPDK_ERRLOG("get_cache_location: key=%ld but block.key=%ld MISMATCH! "
                "seg=%p, idx=%zu, seg->write_ptr=%zu, seg->valid_cnt=%ld, "
                "seg->create_ts=%lu, block.create_ts=%lu, seg->class_num=%d, "
                "block.valid=%d, log_cache_ts=%lu\n",
                key, seg->blocks[idx].key, (void*)seg, idx,
                seg->write_ptr, seg->valid_cnt,
                seg->create_timestamp, seg->blocks[idx].create_timestamp,
                seg->get_class_num(), seg->blocks[idx].valid, log_cache_timestamp);
        return false;
    }

    *offset = block_offset(seg, idx);
    return true;
}

bool LogCache::prepare_gc(GcPrepareResult &result)
{
    // Incremental GC: check if continuing from previous chunk
    bool is_continuation = (result.victim_seg != nullptr && result.scan_offset > 0);

    if (!is_continuation) {
        // New GC - select victim
        if (!need_gc_or_evict()) {
            return false;
        }

        // Determine if we should compact or just evict
        bool compact = false;
        if (target_valid_blk_rate >= 0.1) {
            if (compactor && (double)target_valid_blk_rate * total_cache_block_count > global_valid_blocks) {
                compact = true;
            }
        }

        // Debug: show target_valid_blk_rate and related values
        double current_valid_rate = (double)global_valid_blocks / total_cache_block_count;
        SPDK_NOTICELOG("prepare_gc: compact=%d, target_valid_rate=%.4f, current_valid_rate=%.4f, "
                       "global_valid=%lu, total_blocks=%lu, evict_ratio=%.6f, evict_ghost=%.6f, compact_ratio=%.6f\n",
                       compact, target_valid_blk_rate, current_valid_rate,
                       global_valid_blocks, total_cache_block_count,
                       eviction_ratio.has_value() ? eviction_ratio.value() : -1.0,
                       eviction_ratio_in_ghost_cache.has_value() ? eviction_ratio_in_ghost_cache.value() : -1.0,
                       compaction_ratio.has_value() ? compaction_ratio.value() : -1.0);

        LogCacheSegment* victim = nullptr;
        uint64_t threshold = 0;
        victim = (LogCacheSegment *)evictor->choose_segment();
        if (!victim) {
            return false;
        }
        threshold = log_cache_timestamp - victim->create_timestamp + 1;
        evictor->add(victim, log_cache_timestamp);
        if (compact) {
            victim = (LogCacheSegment *)compactor->choose_segment();
            // ghost compaction 추정: m번째 segment의 valid_cnt 누적
            double g_u = ghost_cache.utilization();
            if (g_u > 0.0) {
                last_ghost_m = static_cast<int>(GHOST_CACHE_RATIO * global_valid_blocks / g_u);
                ghost_compacted_blocks += compactor->get_mth_score_valid_pages(last_ghost_m);
            }
            if (victim->valid_cnt >= 0.8 * victim->blocks.size()) {
                return false;
            }
            if (!victim) {
                return false;
            }
        }
        else {
            return false;
        }

        result.victim_seg = victim;
        result.threshold = threshold;
        result.gc_stream_id = victim->get_class_num();
        result.do_evict_only = !compact;
        result.scan_offset = 0;

        // Set global variables for score_warm_first (async mode)
        g_timestamp = log_cache_timestamp;
        g_threshold = threshold;

        if (victim->valid_cnt == 0) {
            // No valid blocks, just reset
            result.blocks_to_copy.clear();
            result.target_seg = nullptr;
            result.is_final_chunk = true;
            return true;
        }

        // Prepare GC - get target segment for compaction
        result.target_seg = get_segment_to_active_stream(true, result.gc_stream_id);

        // If no target segment available, fall back to evict-only
        if (!result.target_seg) {
            result.do_evict_only = true;
            result.blocks_to_copy.clear();
            result.is_final_chunk = true;
            return true;
        }
    }

    // Incremental scan: process CHUNK_BLOCKS at a time (or full segment if disabled)
    LogCacheSegment* victim = result.victim_seg;
    uint64_t threshold = result.threshold;
    result.blocks_to_copy.clear();

    size_t start = result.scan_offset;
    size_t chunk_size = INCREMENTAL_GC_ENABLED ? GcPrepareResult::CHUNK_BLOCKS : victim->blocks.size();
    size_t end = std::min(start + chunk_size, victim->blocks.size());
    result.chunk_start = start;
    result.scan_offset = end;  // Update now for finalize to use
    result.is_final_chunk = (end >= victim->blocks.size());

    if (result.target_seg) {
        for (std::size_t i = start; i < end; ++i) {
            auto &blk = victim->blocks[i];
            if (!blk.valid) continue;

            // Skip blocks with pending host writes
           // if (pending_writes_.count(blk.key)) continue;

            // Check if should evict or copy based on threshold
            /*if (threshold > 0 && log_cache_timestamp - blk.create_timestamp >= threshold) {
                // Will be evicted, not copied
                continue;
            }*/

            // Check if target segment is full
            if (result.target_seg->full()) {
                evict_policy_add(result.target_seg);
                int assigned_class_num = result.target_seg->get_class_num();
                gc_active_seg.erase(result.target_seg->get_class_num());
                result.target_seg = get_segment_to_active_stream(true, assigned_class_num);

                // If no more segments available, fall back to evict-only
                if (!result.target_seg) {
                    result.do_evict_only = true;
                    result.blocks_to_copy.clear();
                    break;
                }
            }

            GcBlockInfo info;
            info.src_offset = block_offset(victim, i);
            info.dst_idx = result.target_seg->write_ptr;  // Store index for striping
            info.dst_offset = block_offset(result.target_seg, info.dst_idx);
            info.key = blk.key;
            info.src_idx = i;
            info.create_timestamp = blk.create_timestamp;
            info.dst_seg = result.target_seg;  // CRITICAL: Store per-block target segment
            result.blocks_to_copy.push_back(info);

            // Reserve slot in target segment
            result.target_seg->write_ptr++;
        }
    }

    return true;
}

bool LogCache::prepare_evict(EvictPrepareResult &result)
{
    // Incremental evict: check if continuing from previous chunk
    bool is_continuation = (result.victim_seg != nullptr && result.scan_offset > 0);

    if (!is_continuation) {
        // New evict - select victim
        if (!need_gc_or_evict()) {
            return false;
        }

        LogCacheSegment* victim = (LogCacheSegment *)evictor->choose_segment();
        if (!victim) {
            return false;
        }

        result.victim_seg = victim;
        result.scan_offset = 0;

        if (victim->valid_cnt == 0) {
            result.is_final_chunk = true;
            result.chunks.clear();
            return true;
        }
    }

    // Incremental scan: process CHUNK_BLOCKS (16MB) at a time (or full segment if disabled)
    LogCacheSegment* victim = result.victim_seg;
    result.chunks.clear();

    size_t scan_start = result.scan_offset;
    size_t chunk_size = INCREMENTAL_GC_ENABLED ? EvictPrepareResult::CHUNK_BLOCKS : victim->blocks.size();
    size_t scan_end = std::min(scan_start + chunk_size, victim->blocks.size());
    result.chunk_start = scan_start;
    result.scan_offset = scan_end;  // Update now for finalize to use
    result.is_final_chunk = (scan_end >= victim->blocks.size());

    // Group blocks into 128k chunks (32 * 4k) within the 16MB scan range
    constexpr size_t SMALL_CHUNK_BLOCKS = EvictBlockInfo::CHUNK_BLOCKS;  // 32

    for (size_t blk_idx = scan_start; blk_idx < scan_end; ) {
        // Calculate chunk boundary (32-block aligned)
        size_t chunk_start = blk_idx;
        size_t chunk_end = std::min(chunk_start + SMALL_CHUNK_BLOCKS, scan_end);

        // Align to 32-block boundary if not at start
        if (chunk_start % SMALL_CHUNK_BLOCKS != 0) {
            chunk_end = std::min(((chunk_start / SMALL_CHUNK_BLOCKS) + 1) * SMALL_CHUNK_BLOCKS, scan_end);
        }

        // Check if any valid block in this chunk (skip pending writes)
        bool has_valid = false;
        for (size_t i = chunk_start; i < chunk_end; ++i) {
            auto &blk = victim->blocks[i];
            if (!blk.valid) continue;
            // Skip blocks with pending host writes
           // if (pending_writes_.count(blk.key)) continue;
            has_valid = true;
            break;
        }

        if (has_valid) {
            EvictBlockInfo info;
            info.cache_chunk_idx = chunk_start / SMALL_CHUNK_BLOCKS;
            info.valid_mask.resize(SMALL_CHUNK_BLOCKS, false);
            info.backend_keys.resize(SMALL_CHUNK_BLOCKS, 0);
            info.seg_indices.resize(SMALL_CHUNK_BLOCKS, 0);

            for (size_t i = chunk_start; i < chunk_end; ++i) {
                size_t offset_in_chunk = i % SMALL_CHUNK_BLOCKS;
                auto &blk = victim->blocks[i];
                info.seg_indices[offset_in_chunk] = i;
                if (blk.valid) {
                    // Skip blocks with pending host writes
                   // if (pending_writes_.count(blk.key)) continue;
                    info.valid_mask[offset_in_chunk] = true;
                    info.backend_keys[offset_in_chunk] = blk.key;
                }
            }

            result.chunks.push_back(std::move(info));
        }

        blk_idx = chunk_end;
    }

    return true;
}

void LogCache::finalize_gc(GcPrepareResult &result)
{
    LogCacheSegment *victim = result.victim_seg;

    // Update mapping for copied blocks
    for (auto &info : result.blocks_to_copy) {
        auto &src_blk = victim->blocks[info.src_idx];

        // CRITICAL: Use per-block target segment, NOT result.target_seg
        LogCacheSegment *dst_seg = info.dst_seg;
        if (!dst_seg) {
            fprintf(stderr, "BUG: finalize_gc dst_seg=NULL for key=%ld, src_idx=%zu! "
                    "This should never happen - check prepare_gc\n", info.key, info.src_idx);
            // Must erase mapping to avoid mapping->invalid block state
            mapping.erase(info.key);
            src_blk.valid = false;
            global_valid_blocks--;
            continue;
        }

        // Check if mapping still points to victim segment
        // If not, a new write came in during GC - discard GC copy result
        auto it = mapping.find(info.key);
        if (it == mapping.end() || it->second.seg != victim || it->second.idx != info.src_idx) {
            // New write occurred during GC - skip this block (log-structured: new data wins)
            // DEBUG: track skipped blocks for low keys
            if (info.key < 100) {
                SPDK_NOTICELOG("DEBUG finalize_gc SKIP: key=%ld, victim=%p, src_idx=%zu, "
                        "mapping_exists=%d, mapping_seg=%p, mapping_idx=%zu\n",
                        info.key, (void*)victim, info.src_idx,
                        (it != mapping.end()),
                        (it != mapping.end()) ? (void*)it->second.seg : nullptr,
                        (it != mapping.end()) ? it->second.idx : 0);
            }
            src_blk.valid = false;  // Still invalidate source
            continue;
        }

        // Use stored destination index (needed for striping)
        size_t dst_idx = info.dst_idx;

        // Validate dst_idx is within bounds
        if (dst_idx >= dst_seg->blocks.size()) {
            fprintf(stderr, "BUG: finalize_gc dst_idx=%zu >= blocks.size=%zu for key=%ld! "
                    "This should never happen - check prepare_gc\n",
                    dst_idx, dst_seg->blocks.size(), info.key);
            // Must erase mapping to avoid mapping->invalid block state
            mapping.erase(info.key);
            src_blk.valid = false;
            global_valid_blocks--;
            continue;
        }

        // Update target block metadata
        auto &dst_blk = dst_seg->blocks[dst_idx];
        dst_blk.key = info.key;
        dst_blk.valid = true;
        dst_blk.create_timestamp = info.create_timestamp;

        // Update mapping to point to CORRECT target segment
        mapping[info.key] = {dst_seg, dst_idx};

        // Update target segment
        dst_seg->valid_cnt++;
        compacted_blocks++;

        // Invalidate source
        src_blk.valid = false;

        // Update target segment create_timestamp
        if (dst_seg->create_timestamp > info.create_timestamp) {
            dst_seg->create_timestamp = info.create_timestamp;
        }
    }

    // Handle blocks that should be evicted (not copied) - only in current chunk range
    for (std::size_t i = result.chunk_start; i < result.scan_offset; ++i) {
        auto &blk = victim->blocks[i];
        if (!blk.valid) continue;

        // This block was not copied, so it should be evicted
        if (is_ghost_cache) {
            ghost_cache.push(blk.key);
        }
        evicted_blocks += cfg_.evicted_blk_size;
        evicted_timestamp[blk.key] = log_cache_timestamp;
        mapping.erase(blk.key);
        blk.valid = false;
        global_valid_blocks--;
    }

    // Note: target segments will be added to evict policy when they become full

    // Reset victim segment only on final chunk
    if (result.is_final_chunk) {
        reset_segment(victim);

        // Handle stream policy
        if (stream_policy) {
            stream_policy->CollectSegment(victim, log_cache_timestamp);
        }
    }
}

void LogCache::finalize_evict(EvictPrepareResult &result)
{
    LogCacheSegment *victim = result.victim_seg;

    // Invalidate all blocks and update mapping
    for (auto &chunk : result.chunks) {
        for (size_t i = 0; i < chunk.valid_mask.size(); ++i) {
            if (!chunk.valid_mask[i]) continue;

            long key = chunk.backend_keys[i];
            auto it = mapping.find(key);
            // Only invalidate if mapping still points to victim segment
            // If host write came during evict, new data wins - don't touch it
            if (it != mapping.end() && it->second.seg == victim) {
                auto &blk = it->second.seg->blocks[it->second.idx];
                if (blk.valid) {
                    if (is_ghost_cache) {
                        ghost_cache.push(key);
                    }
                    evicted_blocks += cfg_.evicted_blk_size;
                    evicted_timestamp[key] = log_cache_timestamp;
                    blk.valid = false;
                    global_valid_blocks--;
                }
                mapping.erase(it);
            }
        }
    }

    // Reset victim segment only on final chunk
    if (result.is_final_chunk) {
        reset_segment(victim);
    }
}

void LogCache::finalize_gc_async(GcPrepareResult &result, cache_device_io_cb cb, void *cb_arg)
{
    LogCacheSegment *victim = result.victim_seg;
    int compacted_blocks_for_victim = 0;
    int evicted_blocks_for_victim = 0;

    // Update mapping for copied blocks
    for (auto &info : result.blocks_to_copy) {
        auto &src_blk = victim->blocks[info.src_idx];

        // CRITICAL: Use per-block target segment, NOT result.target_seg
        // result.target_seg only points to the LAST segment used, which is wrong
        // when prepare_gc had to allocate multiple target segments
        LogCacheSegment *dst_seg = info.dst_seg;
        if (!dst_seg) {
            SPDK_ERRLOG("BUG: finalize_gc_async dst_seg=NULL for key=%ld, src_idx=%zu\n", info.key, info.src_idx);
            // Must erase mapping to avoid mapping->invalid block state
            mapping.erase(info.key);
            src_blk.valid = false;
            global_valid_blocks--;
            continue;
        }

        // Check if mapping still points to victim segment
        // If not, a new write came in during GC - discard GC copy result
        auto it = mapping.find(info.key);
        if (it == mapping.end() || it->second.seg != victim || it->second.idx != info.src_idx) {
            // New write occurred during GC - skip this block (log-structured: new data wins)
            // DEBUG: track skipped blocks for low keys
            if (info.key < 100) {
                SPDK_NOTICELOG("DEBUG finalize_gc_async SKIP: key=%ld, victim=%p, src_idx=%zu, "
                        "mapping_exists=%d, mapping_seg=%p, mapping_idx=%zu, "
                        "dst_seg=%p, dst_idx=%zu\n",
                        info.key, (void*)victim, info.src_idx,
                        (it != mapping.end()),
                        (it != mapping.end()) ? (void*)it->second.seg : nullptr,
                        (it != mapping.end()) ? it->second.idx : 0,
                        (void*)dst_seg, info.dst_idx);
            }
            src_blk.valid = false;  // Still invalidate source
            continue;
        }

        // Use stored destination index (needed for striping)
        size_t dst_idx = info.dst_idx;

        // Validate dst_idx is within bounds
        if (dst_idx >= dst_seg->blocks.size()) {
            SPDK_ERRLOG("BUG: finalize_gc_async dst_idx=%zu >= blocks.size=%zu for key=%ld\n",
                        dst_idx, dst_seg->blocks.size(), info.key);
            // Must erase mapping to avoid mapping->invalid block state
            mapping.erase(info.key);
            src_blk.valid = false;
            global_valid_blocks--;
            continue;
        }

        // Update target block metadata
        auto &dst_blk = dst_seg->blocks[dst_idx];
        dst_blk.key = info.key;
        dst_blk.valid = true;
        dst_blk.create_timestamp = info.create_timestamp;

        // Update mapping to point to CORRECT target segment
        mapping[info.key] = {dst_seg, dst_idx};

        // Update target segment
        dst_seg->valid_cnt++;
        compacted_blocks++;
        compacted_blocks_for_victim++;

        // Histogram: track compacted block age
        compacted_ages_with_segment_histogram->inc(log_cache_timestamp - info.create_timestamp);

        // Invalidate source
        src_blk.valid = false;

        // Update target segment create_timestamp
        if (dst_seg->create_timestamp > info.create_timestamp) {
            dst_seg->create_timestamp = info.create_timestamp;
        }
    }

    // Handle blocks that should be evicted (not copied) - only in current chunk range
    for (std::size_t i = result.chunk_start; i < result.scan_offset; ++i) {
        auto &blk = victim->blocks[i];
        if (!blk.valid) continue;

        // This block was not copied, so it should be evicted
        if (is_ghost_cache) {
            ghost_cache.push(blk.key);
        }
        evicted_blocks += cfg_.evicted_blk_size;
        evicted_blocks_for_victim++;
        evicted_timestamp[blk.key] = log_cache_timestamp;

        // Histogram: track evicted block age
        evicted_ages_histogram->inc(log_cache_timestamp - blk.create_timestamp);
        evicted_ages_with_segment_histogram->inc(log_cache_timestamp - blk.create_timestamp);

        // DEBUG: track evict for low keys only
        if (blk.key < 100) {
            SPDK_NOTICELOG("DEBUG GC_ASYNC EVICT: key=%ld, victim=%p, victim_idx=%zu\n",
                    blk.key, (void*)victim, i);
        }
        mapping.erase(blk.key);
        blk.valid = false;
        global_valid_blocks--;
    }

    // Histogram: track blocks per chunk (cumulative across chunks)
    evicted_blocks_histogram->inc(evicted_blocks_for_victim);
    compacted_blocks_histogram->inc(compacted_blocks_for_victim);

    // Reset victim segment only on final chunk
    if (result.is_final_chunk) {
        // Handle stream policy before async reset
        if (stream_policy) {
            stream_policy->CollectSegment(victim, log_cache_timestamp);
        }

        SPDK_NOTICELOG("GC_ASYNC: Final chunk, resetting victim=%p\n", (void*)victim);
        // Reset victim segment asynchronously
        reset_segment_async(victim, cb, cb_arg);
    } else {
        // Not final chunk - callback immediately
        if (cb) cb(cb_arg, 0);
    }
}

void LogCache::finalize_evict_async(EvictPrepareResult &result, cache_device_io_cb cb, void *cb_arg)
{
    LogCacheSegment *victim = result.victim_seg;
    int evicted_blocks_for_victim = 0;

    // Invalidate all blocks and update mapping (chunks already contain only current range)
    for (auto &chunk : result.chunks) {
        for (size_t i = 0; i < chunk.valid_mask.size(); ++i) {
            if (!chunk.valid_mask[i]) continue;

            long key = chunk.backend_keys[i];
            auto it = mapping.find(key);
            // Only invalidate if mapping still points to victim segment
            // If host write came during evict, new data wins - don't touch it
            if (it != mapping.end() && it->second.seg == victim) {
                auto &blk = it->second.seg->blocks[it->second.idx];
                if (blk.valid) {
                    if (is_ghost_cache) {
                        ghost_cache.push(key);
                    }
                    evicted_blocks += cfg_.evicted_blk_size;
                    evicted_blocks_for_victim++;
                    evicted_timestamp[key] = log_cache_timestamp;

                    // Histogram: track evicted block age
                    evicted_ages_histogram->inc(log_cache_timestamp - blk.create_timestamp);
                    evicted_ages_with_segment_histogram->inc(log_cache_timestamp - blk.create_timestamp);

                    blk.valid = false;
                    global_valid_blocks--;
                }
                mapping.erase(it);
            }
        }
    }

    // Histogram: track blocks per chunk (cumulative across chunks)
    evicted_blocks_histogram->inc(evicted_blocks_for_victim);

    // Reset victim segment only on final chunk
    if (result.is_final_chunk) {
        reset_segment_async(victim, cb, cb_arg);
    } else {
        // Not final chunk - callback immediately
        if (cb) cb(cb_arg, 0);
    }
}

void LogCache::abort_gc(GcPrepareResult &result)
{
    // Called when GC writes failed - return victim to evictor so it can be retried
    LogCacheSegment *victim = result.victim_seg;
    if (victim) {
        // Return victim to evictor for retry
        evictor->add(victim, log_cache_timestamp);
    }

    // Note: target_seg may have some write_ptr advanced but blocks are not marked valid
    // This creates "holes" in the segment but is safe - they'll be reclaimed later
    // We don't return target to free_pool because it may still be in gc_active_seg
}

void LogCache::abort_evict(EvictPrepareResult &result)
{
    // Called when evict writes failed - return victim to evictor so it can be retried
    LogCacheSegment *victim = result.victim_seg;
    if (victim) {
        // Return victim to evictor for retry
        evictor->add(victim, log_cache_timestamp);
    }
}
