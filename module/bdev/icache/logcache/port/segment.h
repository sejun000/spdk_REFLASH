#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include "emwa.h"

/**
 * 한 세그먼트(≈32 MB)를 구성하는 내부 자료구조
 */
class Segment
{
public:
    Segment(uint64_t create_time) {
        create_timestamp = create_time;
    }
    /* data */
    std::size_t        write_ptr = 0;
    std::size_t        valid_cnt = 0;
    int class_num = 0;
    bool hot = false;
    uint64_t create_timestamp;
    // Host-invalidation rate, in invalidations per host-write page. Each host
    // overwrite/trim of a resident block bumps invalidate_count_ (note_invalidation,
    // O(1)); fold_invalidate_rate() differentiates that count against host time at
    // GS ticks and EWMAs it (same updateFromCumulative pattern as the other ratios).
    // Summed over a WT band at decision time to gauge the old cohort's death rate.
    static constexpr double INVAL_RATE_ALPHA = 0.1;
    uint64_t invalidate_count_      = 0;   // cumulative host-invalidations on this seg
    uint64_t invalidate_prev_count_ = 0;   // count at last fold
    uint64_t invalidate_prev_ts_    = 0;   // host-time at last fold (0 = unseeded)
    Ewma     invalidate_ewma_{INVAL_RATE_ALPHA};   // shared kernel (emwa.h), fixed-α
    /* helpers */
    virtual bool full() = 0;
    virtual void reset() = 0;
    // Current invalidation rate (invalidations per host-write page), EWMA-smoothed.
    double invalidate_rate() const {
        return invalidate_ewma_.has_value() ? invalidate_ewma_.value() : 0.0;
    }
    // Per-event hot path: just bump the cumulative count (O(1), no float work).
    void note_invalidation() { ++invalidate_count_; }
    // Periodic fold (called at GS ticks, same updateFromCumulative pattern as the
    // other ratios): differentiate the cumulative count against host time, EWMA
    // the per-tick rate. First call seeds the baseline (no rate emitted yet).
    void fold_invalidate_rate(uint64_t now) {
        if (invalidate_prev_ts_ == 0) {
            invalidate_prev_ts_    = now;
            invalidate_prev_count_ = invalidate_count_;
            return;
        }
        if (now <= invalidate_prev_ts_) return;            // need dH > 0
        const uint64_t dH = now - invalidate_prev_ts_;
        const uint64_t dU = invalidate_count_ - invalidate_prev_count_;
        invalidate_prev_ts_    = now;
        invalidate_prev_count_ = invalidate_count_;
        invalidate_ewma_.update(static_cast<double>(dU) / static_cast<double>(dH));
    }
    void reset_invalidate_rate() {
        invalidate_ewma_.reset();
        invalidate_count_ = invalidate_prev_count_ = invalidate_prev_ts_ = 0;
    }
    virtual uint64_t get_create_time() {
        return create_timestamp;
    }
    virtual int get_class_num() {
        return class_num;
    }
    static const int GC_STREAM_START = 10;
};
