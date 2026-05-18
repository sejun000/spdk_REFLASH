#include "log_cache.h"
#include "multi_hot_cold.h"
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
extern uint64_t g_threshold;
extern uint64_t g_timestamp;
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
             CacheDeviceInterface *device_io,
             double input_util_step
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
      util_step_(input_util_step),
      evicted_ages_with_segment_histogram(std::make_unique<Histogram>("evicted_ages_with_segment", cache_block_count * 2 / HISTOGRAM_BUCKETS, HISTOGRAM_BUCKETS * 2, fp_stats)),
      compacted_ages_with_segment_histogram(std::make_unique<Histogram>("compacted_ages_with_segment", cache_block_count * 2 / HISTOGRAM_BUCKETS, HISTOGRAM_BUCKETS * 2, fp_stats)),
      gc_copied_lifetime_histogram(std::make_unique<Histogram>("gc_copied_lifetime", cache_block_count * 2 / HISTOGRAM_BUCKETS, HISTOGRAM_BUCKETS * 2, fp_stats)),
      is_ghost_cache(input_ghost_cache),
      compaction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      gc_cost_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      evict_cost_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_compaction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_eviction_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      ghost_reuse_ewma(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      compaction_ratio_in_ghost_cache(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      eviction_ratio_in_ghost_cache(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      flush_avg_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
#if NETFREE_TCO_ENABLED
      netfree_a_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
      netfree_b_ratio(EwmaRatio::FromHalfLifeBlocks(DEFAULT_HALF_LIFE_IN_BLOCKS)),
#endif
      ghost_cache(cache_block_count * util_step_),
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

    // GS_SUM doc spec: EWMA half-life = 1 segment of host writes (= GS_HALF_LIFE_IN_BLOCKS).
    compaction_ratio = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
    eviction_ratio = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
    evict_cost_ratio = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
    ghost_compaction_ratio = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
    ghost_eviction_ratio = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
    ghost_reuse_ewma = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
    compaction_ratio_in_ghost_cache = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
    eviction_ratio_in_ghost_cache = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
    flush_avg_ratio = EwmaRatio::FromHalfLifeBlocks(GS_HALF_LIFE_IN_BLOCKS);
#if NETFREE_TCO_ENABLED
    netfree_a_ratio = EwmaRatio::FromHalfLifeBlocks(segment_size_blocks * 4);
    netfree_b_ratio = EwmaRatio::FromHalfLifeBlocks(segment_size_blocks * 4);
#endif

    evictor->init(&log_cache_timestamp, segment_size_blocks, total_segments);

    if (compactor) {
        compactor->init(&log_cache_timestamp, segment_size_blocks, total_segments);
    }

    stream_policy = input_stream_policy;
    global_valid_blocks = 0;
    g_threshold = total_cache_block_count * 2;

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
            if (loc.seg->blocks[loc.idx].gc_copied_timestamp > 0) {
                gc_copied_lifetime_histogram->inc(log_cache_timestamp - loc.seg->blocks[loc.idx].gc_copied_timestamp);
            }
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
    // Block timestamps are uint32_t — assert we haven't overflowed (~16TB of 4K writes)
    if (!is_ghost_cache) return;
    if (log_cache_timestamp >= UINT32_MAX) {
        fprintf(stderr, "FATAL: log_cache_timestamp (%lu) exceeded uint32_t range. "
                "Block timestamps will be corrupted.\n", log_cache_timestamp);
        abort();
    }

    switch (periodic_mode_) {
    case PeriodicMode::GhostDelta_GC:
        periodic_ghost_delta_gc();
        break;
    case PeriodicMode::NetFree_TCO:
        periodic_netfree_tco();
        break;
    case PeriodicMode::GhostDelta_GC_SUM:
        periodic_ghost_delta_gc_sum();
        break;
    case PeriodicMode::GhostDelta_GC_SUM_Final:
        periodic_ghost_delta_gc_sum_final();
        break;
    }
}

void LogCache::periodic_ghost_delta_gc() {
    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        // Per host-write ratios
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        // Per freed-block ratios (monitoring)
        gc_cost_ratio.updateFromCumulative(gc_freed_blocks, compacted_blocks);
        evict_cost_ratio.updateFromCumulative(evict_freed_blocks, evicted_blocks);
        // Ghost: eviction rate if cache were util_step_ larger
        uint64_t ghost_evicted = ghost_cache.evictCount();
        ghost_eviction_ratio.updateFromCumulative(log_cache_timestamp, ghost_evicted);
        ghost_compaction_ratio.updateFromCumulative(log_cache_timestamp, ghost_compacted_blocks);
        // Ghost reuse rate: d(accessHit) / d(push)
        if (ghost_cache.pushCount() > 0) {
            ghost_reuse_ewma.updateFromCumulative(ghost_cache.pushCount(), ghost_cache.accessHitCount());
        }
    }
    if (log_cache_timestamp % (segment_size_blocks * kGsDecisionPeriodSegs) == 0) {
        if (compaction_ratio.has_value() &&
            eviction_ratio.has_value() &&
            ghost_eviction_ratio.has_value()) {
            double ghost_reuse_rate = ghost_reuse_ewma.has_value() ? ghost_reuse_ewma.value() : 0.0;
            double evict_cost_factor = QLC_TLC_COST_RATIO;
            // GC cost per host write (TLC rewrites)
            double gc_cost = compaction_ratio.value();
            // Eviction savings from having more cache (per host write)
            double evict_savings = (eviction_ratio.value() - ghost_eviction_ratio.value()) * evict_cost_factor;
            double current_valid_rate = (double)global_valid_blocks / total_cache_block_count;

            const char *decision;
            if (evict_savings > gc_cost) {
                // GC saves more than it costs → RAISE
                target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, current_valid_rate + 0.1);
                decision = "RAISE";
            } else {
                // GC costs more than it saves → LOWER
                target_valid_blk_rate = std::max(0.0, current_valid_rate - 0.1);
                decision = "LOWER";
            }
            SPDK_NOTICELOG("periodic: %s target=%.4f, gc_cost=%.6f, evict_savings=%.6f, "
                           "compact_ratio=%.6f, evict_ratio=%.6f, ghost_evict_ratio=%.6f, "
                           "ghost_reuse=%.4f, evict_cost_factor=%.4f, "
                           "gc_cost_ratio=%.6f, evict_cost_ratio=%.6f, "
                           "evicted=%lu, evict_freed=%lu, compacted=%lu, gc_freed=%lu, "
                           "ghost_evicted=%lu, ghost_compacted=%lu\n",
                           decision, target_valid_blk_rate, gc_cost, evict_savings,
                           compaction_ratio.value(), eviction_ratio.value(),
                           ghost_eviction_ratio.value(),
                           ghost_reuse_rate, evict_cost_factor,
                           gc_cost_ratio.has_value() ? gc_cost_ratio.value() : -1.0,
                           evict_cost_ratio.has_value() ? evict_cost_ratio.value() : -1.0,
                           evicted_blocks, evict_freed_blocks, compacted_blocks, gc_freed_blocks,
                           (uint64_t)ghost_cache.evictCount(), ghost_compacted_blocks);
        }
    }
}

void LogCache::periodic_netfree_tco() {
#if NETFREE_TCO_ENABLED
    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        // A = net free segments from GC
        uint64_t A = (gc_victim_count_ > gc_active_alloc_count_)
                     ? (gc_victim_count_ - gc_active_alloc_count_) : 0;

        // Update EWMA: A * segment_size_blocks over log_cache_timestamp
        netfree_a_ratio.updateFromCumulative(log_cache_timestamp,
                                              A * segment_size_blocks);

        // B: accumulate get_mth_score_valid_pages(a_value)
        double a_value = netfree_a_ratio.has_value() ? netfree_a_ratio.value() : 0.0;
        if (compactor && a_value > 0) {
            cumulative_B_ += compactor->get_mth_score_valid_pages(a_value);
        }
        netfree_b_ratio.updateFromCumulative(log_cache_timestamp, cumulative_B_);
    }

    if (log_cache_timestamp % (segment_size_blocks * kGsDecisionPeriodSegs) == 0) {
        if (netfree_a_ratio.has_value() && netfree_b_ratio.has_value()) {
            double a = netfree_a_ratio.value();
            double b = netfree_b_ratio.value();
            double current_valid_rate = (double)global_valid_blocks / total_cache_block_count;

            const char *decision;
            if (b * QLC_TLC_COST_RATIO > a) {
                target_valid_blk_rate = std::min(valid_blk_rate_hard_limit,
                                                  current_valid_rate + 0.1);
                decision = "RAISE";
            } else {
                target_valid_blk_rate = std::max(0.0, current_valid_rate - 0.1);
                decision = "LOWER";
            }
            SPDK_NOTICELOG("periodic_netfree: %s target=%.4f, a=%.6f, b=%.6f, "
                           "b*r=%.6f, gc_victim=%lu, gc_alloc=%lu, cumB=%lu\n",
                           decision, target_valid_blk_rate, a, b,
                           b * QLC_TLC_COST_RATIO,
                           gc_victim_count_, gc_active_alloc_count_, cumulative_B_);
        }
    }
#endif
}

void LogCache::periodic_ghost_delta_gc_sum() {
    // GhostDelta_GC variant: estimate G(u+θ) via cumulative CB-sorted scan.
    //   m = min { k : Σ_{i<k} (seg - v_i) ≥ θ · N · seg }
    //   G(u+θ) cost rate per host write ≈ Σ v_i / Σ (seg-v_i) = u_avg/(1-u_avg)
    // Advance a synthetic ghost_compacted_blocks_sum_ counter at that rate
    // (done in prepare_gc on real compactions); here we just
    // feed cumulative counters into EwmaRatio for rate extraction.

    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(
            log_cache_timestamp,
            static_cast<uint64_t>(ghost_compacted_blocks_sum_));
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        uint64_t evicted_in_ghost = ghost_cache.evictCount();
        eviction_ratio_in_ghost_cache.updateFromCumulative(log_cache_timestamp, evicted_in_ghost);
    }
    if (log_cache_timestamp % (segment_size_blocks * kGsDecisionPeriodSegs) == 0) {
        // Force flush when free_pool is critically low — skip GC copy cost.
        if (free_pool.size() <= FORCE_FLUSH_FREE_SEGMENTS) {
            target_valid_blk_rate = 0.0;
            return;
        }
        if (compaction_ratio.has_value() &&
            compaction_ratio_in_ghost_cache.has_value() &&
            eviction_ratio.has_value() &&
            eviction_ratio_in_ghost_cache.has_value()) {
            double current_valid_rate = (double)global_valid_blocks / total_cache_block_count;
            const char *decision;
            // r · Δflush · backend_waf  vs  Δcomp · fdp_waf
            double delta_flush = eviction_ratio.value() - eviction_ratio_in_ghost_cache.value();
            double delta_comp  = compaction_ratio_in_ghost_cache.value() - compaction_ratio.value();
            double flush_cost = periodic_ratio_ * delta_flush * backend_waf_;
            double comp_cost  = delta_comp * fdp_waf_;
            if (flush_cost > comp_cost) {
                target_valid_blk_rate = std::min(
                    valid_blk_rate_hard_limit,
                    current_valid_rate + util_step_);
                decision = "RAISE";
            } else {
                target_valid_blk_rate = std::max(
                    0.0,
                    current_valid_rate - util_step_);
                decision = "LOWER";
            }
            SPDK_NOTICELOG("periodic_gs: %s target=%.4f, flush_cost=%.6f, comp_cost=%.6f, "
                           "fdp_waf=%.3f, backend_waf=%.3f, "
                           "comp=%.6f, ghost_comp=%.6f, "
                           "evict=%.6f, ghost_evict=%.6f, ghost_comp_sum=%.1f\n",
                           decision, target_valid_blk_rate, flush_cost, comp_cost,
                           fdp_waf_, backend_waf_,
                           compaction_ratio.value(),
                           compaction_ratio_in_ghost_cache.value(),
                           eviction_ratio.value(),
                           eviction_ratio_in_ghost_cache.value(),
                           ghost_compacted_blocks_sum_);
        }
    }
}

// PORTING_GS_FINAL §4.1 — cum_valid 단조 누적, dt × rate 외삽 없음.
// target_free_segs = D = util_step · N_seg (inv_corr 보정 없음, §6.2).
void LogCache::update_ghost_compacted_blocks_sum_cum() {
    if (!compactor) return;
    const double target_free_segs = util_step_ * static_cast<double>(total_segments);
    auto s = compactor->get_ghost_sum_for_free_segments(target_free_segs);
    if (s.cum_invalid > 0.0) {
        ghost_compacted_blocks_sum_ += s.cum_valid;
        ghost_sum_initialized_       = true;
        last_ghost_sum_ts_           = log_cache_timestamp;
    }
}

// PORTING_GS_FINAL §4.2 — LHS = r·waf·F_frac·D vs RHS = G(u+δ).
// flush event 평균 F_frac (D-symmetric with GC cum_valid). target ± util_step_.
void LogCache::periodic_ghost_delta_gc_sum_final() {
    if (!is_ghost_cache) return;

    if (log_cache_timestamp % (segment_size_blocks / 4) == 0) {
        update_ghost_compacted_blocks_sum_cum();
        compaction_ratio.updateFromCumulative(log_cache_timestamp, compacted_blocks);
        compaction_ratio_in_ghost_cache.updateFromCumulative(
            log_cache_timestamp,
            static_cast<uint64_t>(ghost_compacted_blocks_sum_));
        eviction_ratio.updateFromCumulative(log_cache_timestamp, evicted_blocks);
        flush_avg_ratio.updateFromCumulative(
            flush_event_count_ * segment_size_blocks, evicted_blocks);
    }

    if (log_cache_timestamp % segment_size_blocks == 0) {
        // Force-flush guard (Phase 2): preserved on FINAL path too (porting doc §6.4).
        if (free_pool.size() <= FORCE_FLUSH_FREE_SEGMENTS) {
            target_valid_blk_rate = 0.0;
            return;
        }
        if (!compaction_ratio.has_value() ||
            !compaction_ratio_in_ghost_cache.has_value() ||
            !eviction_ratio.has_value()) return;

        const double waf_w  = (backend_waf_ > 0.0) ? backend_waf_ : 1.0;
        const double Gud    = compaction_ratio_in_ghost_cache.value();
        const double F_frac = flush_avg_ratio.has_value()
                            ? flush_avg_ratio.value() : 1.0;   // §6.3 conservative fallback

        const double lhs = periodic_ratio_ * waf_w * F_frac
                         * util_step_ * static_cast<double>(total_segments);
        const double rhs = Gud;
        const bool   raise = (lhs > rhs);

        const double cur_util = (total_cache_block_count > 0)
                              ? (double)global_valid_blocks / total_cache_block_count : 0.0;
        const double raw_target = raise ? (cur_util + util_step_) : (cur_util - util_step_);

        if (raise) target_valid_blk_rate = std::min(valid_blk_rate_hard_limit, raw_target);
        else       target_valid_blk_rate = std::max(0.0, raw_target);

        SPDK_NOTICELOG("periodic_gs_final: %s target=%.4f, lhs=%.6f, rhs=%.6f, "
                       "F_frac=%.4f, backend_waf=%.3f, Gud=%.6f, flush_events=%lu, sum=%.1f\n",
                       raise ? "RAISE" : "LOWER",
                       target_valid_blk_rate, lhs, rhs,
                       F_frac, backend_waf_, Gud, flush_event_count_,
                       ghost_compacted_blocks_sum_);
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
    blk.create_timestamp = static_cast<uint32_t>(log_cache_timestamp);
    mapping[key] = { seg, seg->write_ptr };

    ++seg->write_ptr;
    ++seg->valid_cnt;
    ++global_valid_blocks;
    ++log_cache_timestamp;
    g_timestamp = log_cache_timestamp;
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
    blk.create_timestamp = static_cast<uint32_t>(log_cache_timestamp);
    mapping[key] = { seg, seg->write_ptr };
    pending_writes_.insert(key);  // Mark as write in progress

    ++seg->write_ptr;
    ++seg->valid_cnt;
    ++global_valid_blocks;
    ++log_cache_timestamp;
    g_timestamp = log_cache_timestamp;
    if (stream_policy) {
        stream_policy->Append(key, log_cache_timestamp, reinterpret_cast<void*>(seg->valid_cnt));
    }
    
    write_size_to_cache += lba_sz;

    *cache_offset = dst_offset;
    // Return placement handle for FDP
    if (out_stream_id) {
        if (stream_policy) {
            // Use stream policy's classification as placement handle
            *out_stream_id = seg->get_class_num();
        } else {
            // Legacy toggle mode
            *out_stream_id = host_write_handle_;
        }
    }

    // Check if segment became full after this write - add to evictor immediately
    // This prevents full segments from staying in active_seg when next write goes to different stream
    bool segment_full = seg->full();
    if (segment_full) {
        evict_policy_add(seg);
        active_seg.erase(seg->get_class_num());
        // Toggle only when not using stream_policy (legacy mode)
        if (!stream_policy) {
            toggle_host_write_handle();
        }
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

    // Track GC segment allocations (shrink=false means GC allocation)
    if (!shrink) {
        gc_segments_allocated++;
#if NETFREE_TCO_ENABLED
        gc_active_alloc_count_++;
#endif
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
    if (exists(key))
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

    // Cycle wrap detected → dummy fill old active GC segments before reuse
    if (gc) {
        int victim_id = stream_policy->GetVictimStreamId(log_cache_timestamp, 0);
        while (victim_id >= Segment::GC_STREAM_START) {
            auto vit = gc_active_seg.find(victim_id);
            if (vit != gc_active_seg.end()) {
                SPDK_NOTICELOG("Cycle wrap: dummy fill stream %d, seg=%p, write_ptr=%zu\n",
                        victim_id, (void*)vit->second, vit->second->write_ptr);
                dummy_fill_segment(vit->second);
                gc_active_seg.erase(vit);
            }
            victim_id = stream_policy->GetVictimStreamId(log_cache_timestamp, 0);
        }
    }

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
            s->blocks[i].create_timestamp = UINT32_MAX;
        }
        s->write_ptr = s->blocks.size();
        evict_policy_add(s);
    }
}



Segment* LogCache::evict_and_compaction(LogCacheSegment* s, uint64_t threshold, int gc_stream_id)
{
#if NETFREE_TCO_ENABLED
    gc_victim_count_++;
#endif
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
    // Track net freed blocks from GC: segment freed minus compacted blocks that consume space in target
    gc_freed_blocks += (segment_size_blocks - compacted_blocks_for_victim);
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

        evict(s, i);

    }
    evict_freed_blocks += segment_size_blocks;
    reset_segment(s);
    // PORTING_GS_FINAL §5: ++flush_event_count_ at end of evict_segment.
    // Drives flush_avg_ratio sample = evicted_blocks / (flush_events × seg_blocks).
    ++flush_event_count_;
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
    if (evicted_ages_with_segment_histogram) evicted_ages_with_segment_histogram->print_current(reset);
    if (compacted_ages_with_segment_histogram) compacted_ages_with_segment_histogram->print_current(reset);
    if (gc_copied_lifetime_histogram) gc_copied_lifetime_histogram->print_current(reset);
}

void LogCache::log_victim_age_dist(const char *label, LogCacheSegment *victim) {
    if (!fp_stats || !victim || victim->valid_cnt == 0) return;
    constexpr int NUM_BUCKETS = 8;
    uint64_t age_min = UINT64_MAX, age_max = 0, age_sum = 0, valid_count = 0;
    uint64_t buckets[NUM_BUCKETS] = {};
    for (size_t i = 0; i < victim->blocks.size(); ++i) {
        if (!victim->blocks[i].valid) continue;
        uint64_t age = log_cache_timestamp - victim->blocks[i].create_timestamp;
        if (age < age_min) age_min = age;
        if (age > age_max) age_max = age;
        age_sum += age;
        valid_count++;
    }
    if (valid_count == 0) return;
    uint64_t range = age_max - age_min + 1;
    uint64_t bucket_width = (range + NUM_BUCKETS - 1) / NUM_BUCKETS;
    if (bucket_width == 0) bucket_width = 1;
    double mean = (double)age_sum / valid_count;
    double sq_sum = 0;
    for (size_t i = 0; i < victim->blocks.size(); ++i) {
        if (!victim->blocks[i].valid) continue;
        uint64_t age = log_cache_timestamp - victim->blocks[i].create_timestamp;
        int b = (int)((age - age_min) / bucket_width);
        if (b >= NUM_BUCKETS) b = NUM_BUCKETS - 1;
        buckets[b]++;
        double diff = (double)age - mean;
        sq_sum += diff * diff;
    }
    double stddev = sqrt(sq_sum / valid_count);
    fprintf(fp_stats, "histogram,%s\n", label);
    fprintf(fp_stats, "timestamp,%lu,class,%d,valid,%lu,total,%zu,mean,%.0f,stddev,%.0f\n",
            log_cache_timestamp, victim->get_class_num(), valid_count, victim->blocks.size(), mean, stddev);
    fprintf(fp_stats, "bucket,age_min,age_max,count\n");
    for (int b = 0; b < NUM_BUCKETS; b++) {
        uint64_t bmin = age_min + (uint64_t)b * bucket_width;
        uint64_t bmax = bmin + bucket_width - 1;
        if (b == NUM_BUCKETS - 1)
            fprintf(fp_stats, "%d,%lu,+inf,%lu\n", b, bmin, buckets[b]);
        else
            fprintf(fp_stats, "%d,%lu,%lu,%lu\n", b, bmin, bmax, buckets[b]);
    }
    fprintf(fp_stats, "\n");
    fflush(fp_stats);

    // Append to victim_ratio CSV
    if (fp_victim_ratio) {
        double valid_ratio = (double)valid_count / victim->blocks.size();
        fprintf(fp_victim_ratio, "%lu,%s,%d,%lu,%zu,%.6f\n",
                log_cache_timestamp, label, victim->get_class_num(),
                valid_count, victim->blocks.size(), valid_ratio);
        fflush(fp_victim_ratio);
    }
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
        // PORTING_GS_FINAL §5: gate uses util_step_ for FINAL; legacy 0.1 elsewhere.
        const double compact_gate_threshold =
            (periodic_mode_ == PeriodicMode::GhostDelta_GC_SUM_Final) ? util_step_ : 0.1;
        bool compact = false;
        if (target_valid_blk_rate >= compact_gate_threshold) {
            if (compactor && (double)target_valid_blk_rate * total_cache_block_count > global_valid_blocks) {
                compact = true;
            }
        }

        // Debug: show target_valid_blk_rate and related values
        double current_valid_rate = (double)global_valid_blocks / total_cache_block_count;
        SPDK_NOTICELOG("prepare_gc: compact=%d, target_valid_rate=%.4f, current_valid_rate=%.4f, "
                       "global_valid=%lu, total_blocks=%lu, evict_ratio=%.6f, compact_ratio=%.6f\n",
                       compact, target_valid_blk_rate, current_valid_rate,
                       global_valid_blocks, total_cache_block_count,
                       eviction_ratio.has_value() ? eviction_ratio.value() : -1.0,
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
            if (victim->valid_cnt > 0.95 * segment_size_blocks) {
                compactor->add(victim, log_cache_timestamp);
                return false;
            }
            // Ghost compaction estimate: cost of GC if cache were util_step_ larger
            if (periodic_mode_ == PeriodicMode::GhostDelta_GC) {
                double g_u = ghost_reuse_ewma.has_value()
                             ? ghost_reuse_ewma.value()
                             : ghost_cache.utilization();
                if (g_u > 0.0) {
                    double m = util_step_ * total_cache_block_count / g_u / segment_size_blocks;
                    uint64_t valid_cost = compactor->get_kth_segment_valid_cnt_for_free_segments(m);
                    ghost_compacted_blocks += valid_cost;
                    ghost_gc_freed_blocks += (segment_size_blocks - valid_cost);
                }
            }

            // GS variant: ghost_compacted_blocks_sum_ advances at the
            // θ-regime rate u_avg/(1-u_avg), but ONLY at real compaction
            // events. No real comp → no ghost tick → Δcomp = 0.
            if (periodic_mode_ == PeriodicMode::GhostDelta_GC_SUM) {
                double inv_corr = 1.0;
                if (ghost_sum_initialized_ && log_cache_timestamp > last_ghost_sum_ts_) {
                    const uint64_t win_writes = log_cache_timestamp - last_ghost_sum_ts_;
                    const uint64_t win_inv    = (invalidate_blocks > last_invalidate_at_comp_)
                                              ? (invalidate_blocks - last_invalidate_at_comp_)
                                              : 0;
                    const double i_rate = std::min(0.95,
                        static_cast<double>(win_inv) / static_cast<double>(win_writes));
                    const double theta_i = util_step_ * i_rate;
                    inv_corr = 1.0 / (1.0 - std::min(0.95, theta_i));
                }
                const double target_free_segs = util_step_ *
                    static_cast<double>(total_segments) * inv_corr;
                auto s = compactor->get_ghost_sum_for_free_segments(target_free_segs);
                if (s.cum_invalid > 0.0) {
                    if (ghost_sum_initialized_) {
                        const double rate = s.cum_valid / s.cum_invalid;
                        const uint64_t dt = (log_cache_timestamp > last_ghost_sum_ts_)
                                          ? (log_cache_timestamp - last_ghost_sum_ts_)
                                          : 0;
                        // Anchor to compacted_blocks each tick (per doc §6.1).
                        ghost_compacted_blocks_sum_ =
                            static_cast<double>(compacted_blocks)
                            + static_cast<double>(dt) * rate;
                    } else {
                        // First real comp: anchor base, dt=0 to avoid spike.
                        ghost_compacted_blocks_sum_ =
                            static_cast<double>(compacted_blocks);
                        ghost_sum_initialized_ = true;
                    }
                    last_ghost_sum_ts_       = log_cache_timestamp;
                    last_invalidate_at_comp_ = invalidate_blocks;
                }
            }
            if (!victim) {
                return false;
            }
        }
        else {
            return false;
        }

#if NETFREE_TCO_ENABLED
        gc_victim_count_++;
#endif
        result.victim_seg = victim;
        result.threshold = threshold;
        result.gc_stream_id = victim->get_class_num();
        result.do_evict_only = !compact;
        result.scan_offset = 0;

        // Set global variables for score_warm_first (async mode)
        g_timestamp = log_cache_timestamp;
        g_threshold = threshold; //+ segment_size_blocks;

        // log_victim_age_dist("gc_victim_age_dist", victim);

        if (victim->valid_cnt == 0) {
            // No valid blocks, just reset
            result.blocks_to_copy.clear();
            result.target_seg = nullptr;
            result.is_final_chunk = true;
            return true;
        }

        // Prepare GC - get target segment for compaction
        if (!stream_policy) {
            result.target_seg = get_segment_to_active_stream(true, result.gc_stream_id);
            // If no target segment available, fall back to evict-only
            if (!result.target_seg) {
                result.do_evict_only = true;
                result.blocks_to_copy.clear();
                result.is_final_chunk = true;
                return true;
            }
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

    if (result.target_seg || stream_policy) {
        for (std::size_t i = start; i < end; ++i) {
            auto &blk = victim->blocks[i];
            if (!blk.valid) continue;

            // Per-block stream classification
            LogCacheSegment *target_seg = nullptr;
            if (stream_policy) {
                target_seg = get_segment_with_stream_policy(true, blk.key);
            } else {
                target_seg = result.target_seg;
            }

            if (!target_seg) {
                result.do_evict_only = true;
                result.blocks_to_copy.clear();
                break;
            }

            // Check if target segment is full
            if (target_seg->full()) {
                evict_policy_add(target_seg);
                int assigned_class_num = target_seg->get_class_num();
                gc_active_seg.erase(target_seg->get_class_num());
                target_seg = get_segment_to_active_stream(true, assigned_class_num);

                // If no more segments available, fall back to evict-only
                if (!target_seg) {
                    SPDK_ERRLOG("BUG: No target segment available during incremental GC\n");
                    result.do_evict_only = true;
                    result.blocks_to_copy.clear();
                    break;
                }
            }

            // Update result.target_seg for non-stream-policy path
            if (!stream_policy) {
                result.target_seg = target_seg;
            }

            GcBlockInfo info;
            info.src_offset = block_offset(victim, i);
            info.dst_idx = target_seg->write_ptr;  // Store index for striping
            info.dst_offset = block_offset(target_seg, info.dst_idx);
            info.key = blk.key;
            info.src_idx = i;
            info.create_timestamp = blk.create_timestamp;
            info.gc_copied_timestamp = blk.gc_copied_timestamp;
            info.dst_seg = target_seg;  // Per-block target segment (stream-classified)
            result.blocks_to_copy.push_back(info);

            // Reserve slot in target segment
            target_seg->write_ptr++;
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

        // log_victim_age_dist("evict_victim_age_dist", victim);

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
    int compacted_blocks_for_victim = 0;

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
        dst_blk.gc_copied_timestamp = info.gc_copied_timestamp ? info.gc_copied_timestamp : static_cast<uint32_t>(log_cache_timestamp);

        // Update mapping to point to CORRECT target segment
        mapping[info.key] = {dst_seg, dst_idx};

        // Update target segment
        dst_seg->valid_cnt++;
        compacted_blocks++;
        compacted_blocks_for_victim++;

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
        mapping.erase(blk.key);
        blk.valid = false;
        global_valid_blocks--;
    }

    // Note: target segments will be added to evict policy when they become full

    // Reset victim segment only on final chunk
    if (result.is_final_chunk) {
        gc_freed_blocks += (segment_size_blocks - compacted_blocks_for_victim);
        reset_segment(victim);

        // Handle stream policy
     /*  if (stream_policy) {
            stream_policy->CollectSegment(victim, log_cache_timestamp);
        }*/
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
                    blk.valid = false;
                    global_valid_blocks--;
                }
                mapping.erase(it);
            }
        }
    }

    // Reset victim segment only on final chunk
    if (result.is_final_chunk) {
        evict_freed_blocks += segment_size_blocks;
        reset_segment(victim);
    }
}

void LogCache::finalize_gc_async(GcPrepareResult &result, cache_device_io_cb cb, void *cb_arg)
{
    LogCacheSegment *victim = result.victim_seg;
    int compacted_blocks_for_victim = 0;
    int evicted_blocks_for_victim = 0;

    // Collect victim segment lifespan BEFORE GC append/classify
    // so that mAvgLifespan is up-to-date for stream classification
    if (stream_policy) {
        stream_policy->CollectSegment(victim, log_cache_timestamp);
    }

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
        dst_blk.gc_copied_timestamp = info.gc_copied_timestamp ? info.gc_copied_timestamp : static_cast<uint32_t>(log_cache_timestamp);

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

        evicted_ages_with_segment_histogram->inc(log_cache_timestamp - blk.create_timestamp);
        if (blk.gc_copied_timestamp > 0) {
            gc_copied_lifetime_histogram->inc(log_cache_timestamp - blk.gc_copied_timestamp);
        }

        // DEBUG: track evict for low keys only
        if (blk.key < 100) {
            SPDK_NOTICELOG("DEBUG GC_ASYNC EVICT: key=%ld, victim=%p, victim_idx=%zu\n",
                    blk.key, (void*)victim, i);
        }
        mapping.erase(blk.key);
        blk.valid = false;
        global_valid_blocks--;
    }

    // Reset victim segment only on final chunk
    if (result.is_final_chunk) {
        gc_freed_blocks += (segment_size_blocks - compacted_blocks_for_victim);
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

                    evicted_ages_with_segment_histogram->inc(log_cache_timestamp - blk.create_timestamp);
                    if (blk.gc_copied_timestamp > 0) {
                        gc_copied_lifetime_histogram->inc(log_cache_timestamp - blk.gc_copied_timestamp);
                    }

                    blk.valid = false;
                    global_valid_blocks--;
                }
                mapping.erase(it);
            }
        }
    }

    // Reset victim segment only on final chunk
    if (result.is_final_chunk) {
        evict_freed_blocks += segment_size_blocks;
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
