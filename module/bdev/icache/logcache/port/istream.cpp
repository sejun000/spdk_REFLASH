#include "istream.h"
#include "sepbit.h"
#include "hot_cold.h"
#include "multi_hot_cold.h"
#include "hot_cold_midas.h"
#include <string>
#include <cassert>
#include <algorithm>

uint64_t interval = 1;
uint64_t g_stream_fallback_blocks = 0;
uint64_t g_stream_segment_size_blocks = 0;
namespace {
constexpr int kMultiHotColdStreams = 5;
}

// g_threshold is owned by icache.cpp (extern declared via log_cache.cpp).
extern uint64_t g_threshold;

void set_stream_interval(uint64_t cache_block_count, uint64_t segment_size_blocks) {
    uint64_t computed = (uint64_t)(cache_block_count / (3));
    if (computed == 0) {
        computed = 1;
    }
    // Align interval to segment boundary if segment_size_blocks is specified
    if (segment_size_blocks > 0) {
        // Round up to nearest segment boundary
        computed = ((computed + segment_size_blocks - 1) / segment_size_blocks) * segment_size_blocks;
        if (computed == 0) {
            computed = segment_size_blocks;
        }
    }
    interval = computed;
    g_stream_fallback_blocks = cache_block_count;
    g_stream_segment_size_blocks = segment_size_blocks;
}

uint64_t compute_stream_interval(uint64_t fallback_cache_blocks) {
    uint64_t base = (g_threshold > 0)
                  ? g_threshold
                  : (fallback_cache_blocks > 0
                      ? fallback_cache_blocks
                      : g_stream_fallback_blocks);
    if (base == 0) return interval;  // keep prior

    uint64_t computed = base / kMultiHotColdStreams;
    if (computed == 0) computed = 1;
    if (g_stream_segment_size_blocks > 0) {
        uint64_t seg = g_stream_segment_size_blocks;
        computed = ((computed + seg - 1) / seg) * seg;
        if (computed == 0) computed = seg;
    }
    return computed;
}

IStream* createIstreamPolicy(std::string policy_type) {
    if (policy_type == "none" || policy_type.empty()) {
        return nullptr;
    }
    if (policy_type == "sepbit") {
        return new SepBIT();
    }
    else if (policy_type == "hotcold") {
        return new HotCold();
    } else if (policy_type == "multi_hotcold") {
        return new MultiHotCold(kMultiHotColdStreams, interval, false);
    }
    else if (policy_type == "multi_hotcold_create_timestamp_only") {
        return new MultiHotCold(kMultiHotColdStreams, interval, true);
    }
    else if (policy_type == "multi_hotcold_2") {
        return new MultiHotCold(kMultiHotColdStreams, interval, true, true, false);
    }
    else if (policy_type == "multi_hotcold_3") {
        return new MultiHotCold(kMultiHotColdStreams, interval, true, true, true);
    }
    else if (policy_type == "midas_hotcold") {
        return new MiDASHotCold();
    }
    else {
        assert(false);
    }
}
