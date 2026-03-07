#include "icache.h"
#include "log_cache.h"
#include "evict_policy_fifo.h"
#include "evict_policy_fifo_zero.h"
#include "evict_policy_greedy.h"
#include "evict_policy_cost_benefit.h"
#include "evict_policy_lambda.h"
#include "evict_policy_selective_fifo.h"
#include "evict_policy_k_cost_benefit.h"
#include "evict_policy_multiqueue.h"
#include "evict_policy_midas.h"
#include "istream.h"
#include "multi_hot_cold.h"
#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdlib>

#define TEN_GB (10 * 1024ULL * 1024ULL * 1024ULL)

static double score_hot_and_greedy(Segment *seg) {
    return -seg->valid_cnt;
}


static double score_age_evict(Segment *seg) {
    return -seg->create_timestamp;
}
double segments = 8192.0;
static double score_age(Segment *seg) {
    if (seg->valid_cnt > segments - 2) {
        return -UINT64_MAX;
    }
    return seg->create_timestamp;
}

uint64_t g_threshold = 0;
uint64_t g_timestamp = 1;

// Check if segment is from an old cycle for its stream → protect from compaction
// Uses seg->create_timestamp (= oldest block's timestamp after compaction) instead of seg->cycle
static inline bool is_old_cycle_segment(Segment *seg) {
    if (g_cycle_length == 0 || interval == 0) return false;
    int seg_cycle = static_cast<int>(seg->create_timestamp / g_cycle_length);
    int idx = seg->class_num - Segment::GC_STREAM_START;
    if (idx < 0 || idx >= IStream::MAX_STREAMS) {
        // Host segment: estimate which GC stream its blocks would map to
        idx = static_cast<int>((seg->create_timestamp % g_cycle_length) / interval);
    }
    if (idx >= 0 && idx < IStream::MAX_STREAMS) {
        if (seg_cycle < g_stream_cycles[idx]) {
            return true;
        }
    }
    return false;
}

static double score_hot_first(Segment *seg) {
    if (is_old_cycle_segment(seg)) return 0.0;
    assert(g_threshold > 0 && g_timestamp > 0);
    double u = seg->valid_cnt / segments;
    return (g_threshold - (g_timestamp - seg->create_timestamp)) * (1 - u) / (u);
}


static double score_cold_first(Segment *seg) {
    if (is_old_cycle_segment(seg)) return 0.0;
    assert(g_threshold > 0 && g_timestamp > 0);
    double u = seg->valid_cnt / segments;
    return (g_timestamp - seg->create_timestamp) * (1 - u) / (u);
}

static double score_warm_first(Segment *seg) {
    if (is_old_cycle_segment(seg)) return 0.0;
    if (g_threshold <= 0 || g_timestamp <= 0) {
        //printf("g_threshold %d g_timestamp %d\n", g_threshold, g_timestamp);
        assert(g_threshold > 0 && g_timestamp > 0);
    }
    double u = seg->valid_cnt / segments;
    return std::min(g_threshold - (g_timestamp - seg->create_timestamp), g_timestamp - seg->create_timestamp) * (1 - u) / (u);
}

static double score_warm_first_hot_last(Segment *seg) {
    if (is_old_cycle_segment(seg)) return 0.0;
    if (g_threshold <= 0 || g_timestamp <= 0) {
        //printf("g_threshold %d g_timestamp %d\n", g_threshold, g_timestamp);
        assert(g_threshold > 0 && g_timestamp > 0);
    }
    double u = seg->valid_cnt / segments;
    if (seg->hot) {
        return (g_timestamp - seg->create_timestamp) * (1 - u) / u;
    }
    return std::min(g_threshold - (g_timestamp - seg->create_timestamp), g_timestamp - seg->create_timestamp) * (1 - u) / (u);
}

static double score_sepbit_age(Segment *seg) {
    assert(g_threshold > 0 && g_timestamp > 0);
    double u = seg->valid_cnt / segments;
    return (g_timestamp - seg->create_timestamp) * (1 - u) / (u);
}


int g_numerator = 30;
int g_denominator = 90;

static std::size_t ranking(std::size_t N) {
    
    std::size_t target_rank = (g_numerator * N) / g_denominator;
    
    //printf ("rank idx %ld\n", target_rank);
    //printf("rank N %d g_numerator %d g_denominator %d target_ranking_score %d ret_rank %d \n", N, g_numerator, g_denominator, (g_numerator * N) / g_denominator, ret_rank);
    return target_rank;
}

namespace {
template <typename T>
T* attach_prefix(T* cache, const std::string& prefix) {
    if (cache) {
        cache->set_stats_prefix(prefix);
    }
    return cache;
}
}

ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity, int cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file, std::string &waf_log_file, double valid_rate_threshold, std::string stat_log_file) {
    if (capacity <= 0) {
        capacity = 1;
    }
    set_stream_interval(static_cast<uint64_t>(capacity));
    if (cache_type == "LOG_FIFO") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>()), cache_type);
    }
    else if (cache_type == "LOG_FIFO_ZERO") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoZeroEvictPolicy>()), cache_type);
    }
    else if (cache_type == "LOG_GREEDY") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file), cache_type);
    }
    else if (cache_type == "LOG_COST_BENEFIT") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>()), cache_type);
    }
    else if (cache_type == "LOG_MIDAS_DEFAULT") {
        IStream *input_stream_policy = createIstreamPolicy("midas_hotcold");
        auto cache = new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace,
                                  trace_file, cold_trace_file, waf_log_file,
                                  std::make_unique<MiDASGreedyEvictPolicy>(),
                                  nullptr, input_stream_policy);
        return attach_prefix(cache, cache_type);
    }
    else if (cache_type == "LOG_LAMBDA") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<LambdaEvictPolicy>()), cache_type);
    }
    else if (cache_type == "LOG_FIFO_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), nullptr, input_stream_policy), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_SEPBIT"){
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<GreedyEvictPolicy>(), nullptr, input_stream_policy), cache_type);
    }
    else if (cache_type == "LOG_COST_BENEFIT_SEPBIT") { 
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(), nullptr, input_stream_policy), cache_type);
    }
    else if (cache_type == "LOG_SELECTIVE_FIFO_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), nullptr, input_stream_policy), cache_type);
    }
    else if (cache_type == "LOG_FIFO_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), nullptr, input_stream_policy), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<GreedyEvictPolicy>(), nullptr, input_stream_policy), cache_type);
    }
    else if (cache_type == "LOG_COST_BENEFIT_HOTCOLD") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(), nullptr, input_stream_policy), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_SELECTIVE_FIFO_0_7") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, nullptr, 0.7, std::make_unique<GreedyEvictPolicy>()), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_SELECTIVE_FIFO") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, nullptr, 0.85, std::make_unique<GreedyEvictPolicy>()), cache_type);
    }
    else if (cache_type == "LOG_MULTI_QUEUE") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file,
            cold_trace_file, waf_log_file, std::make_unique<MultiQueueEvictPolicy>(), nullptr, nullptr), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_FIFO") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(),
            nullptr, nullptr, 0.93, std::make_unique<GreedyEvictPolicy>(), 1.2), cache_type);
    }
    else if (cache_type == "LOG_HOT_FIRST_SELECTIVE_FIFO_0_6_SEPBIT") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(), 
            nullptr, input_stream_policy, 0.8, std::make_unique<CbEvictPolicy>(score_hot_and_greedy)), cache_type);
    }
    else if (cache_type == "LOG_1TH_COST_BENEFIT") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 0.7), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<GreedyEvictPolicy>(), 0.7), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_2") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_create_timestamp_only");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<GreedyEvictPolicy>(), 0.5, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_3") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_create_timestamp_only");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_hot_first), 1.2, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_4") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<CbEvictPolicy>(score_hot_first), 1.2, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_4_4") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<CbEvictPolicy>(score_cold_first), 1.2, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_5") {
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_6") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_2");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_7") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_warm_first), 1.2, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_8") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_8_2") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<CbEvictPolicy>(score_cold_first), 0, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_8_3") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.90, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_9") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.8, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_10") {
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.6, std::make_unique<CbEvictPolicy>(score_warm_first), 0, true), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_11") { // for getting optimized value from dynamic algorithm
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, valid_rate_threshold, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false, stat_log_file), cache_type);
    }
    else if (cache_type == "LOG_FIFO_2") {
        Config cfg;
        cfg.segment_bytes = 32ull * 1024 * 1024;
        cfg.free_ratio_low = 0.01;
        cfg.evicted_blk_size = 16;
        cfg.print_stats_interval = TEN_GB;
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), &cfg), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_12") { // for 64k eviction
        Config cfg;
        cfg.segment_bytes = 32ull * 1024 * 1024;
        cfg.free_ratio_low = 0.01;
        cfg.evicted_blk_size = 16;
        cfg.print_stats_interval = TEN_GB;
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            &cfg, input_stream_policy, 0.8, std::make_unique<CbEvictPolicy>(score_warm_first), 0, false), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_COST_BENEFIT_13") { // for 64k eviction
        Config cfg;
        cfg.segment_bytes = 32ull * 1024 * 1024;
        cfg.free_ratio_low = 0.01;
        cfg.evicted_blk_size = 16;
        cfg.print_stats_interval = TEN_GB;
        IStream *input_stream_policy = createIstreamPolicy("multi_hotcold_3");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            &cfg, input_stream_policy, 0.6, std::make_unique<CbEvictPolicy>(score_warm_first), 0, true), cache_type);
    }
    else if (cache_type == "LOG_SEPBIT_FIFO") {
        
        IStream *input_stream_policy = createIstreamPolicy("sepbit");
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<FifoEvictPolicy>(), nullptr, input_stream_policy,
            0.8, std::make_unique<CbEvictPolicy>(score_sepbit_age), 0), cache_type);
    }
    else if (cache_type == "LOG_GREEDY_FIFO_2") {
        g_numerator = 10;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(),
            nullptr, nullptr, 0.8, std::make_unique<GreedyEvictPolicy>(), 0), cache_type);
    }
    else if (cache_type == "LOG_LAST_COST_BENEFIT") {
        g_numerator = 90;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.0), cache_type);
    }
    else if (cache_type == "LOG_SELECTIVE_FIFO_2") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        std::vector<int> v;
        v.insert(v.end(), {0, 1, 2, 3});
        std::vector<int> v2;
        v2.insert(v2.end(), {3, 2, 1, 0});
        // v의 복사본을 가지는 unique_ptr를 생성하여 전달
        auto v_ptr = std::make_unique<std::vector<int>>(v);
        auto v_ptr2 = std::make_unique<std::vector<int>>(v2);
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<SelectiveFifoEvictPolicy>(true, false, std::move(v_ptr2)), 
            nullptr, input_stream_policy, 0.93, std::make_unique<SelectiveFifoEvictPolicy>(true, true, std::move(v_ptr)), 0.5), cache_type);
    }
    else if (cache_type == "LOG_SELECTIVE_FIFO_3") {
        IStream *input_stream_policy = createIstreamPolicy("hotcold");
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        std::vector<int> v;
        v.insert(v.end(), {0, 1, 2, 3});
        std::vector<int> v2;
        v2.insert(v2.end(), {3, 2, 1, 0});
        // v의 복사본을 가지는 unique_ptr를 생성하여 전달
        auto v_ptr = std::make_unique<std::vector<int>>(v);
        auto v_ptr2 = std::make_unique<std::vector<int>>(v2);
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, input_stream_policy, 0.93, std::make_unique<SelectiveFifoEvictPolicy>(true, true, std::move(v_ptr)), 0.5), cache_type);
    }
    else if (cache_type == "LOG_5TH_COST_BENEFIT") {
        g_numerator = 50;
        g_denominator = 90;        
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.2), cache_type);
    }
    else if (cache_type == "LOG_8TH_COST_BENEFIT") {
        g_numerator = 80;
        g_denominator = 90;
        /*return new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<KthCbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.85, std::make_unique<SelectiveFifoEvictPolicy>(), 0.15);*/
            
        return attach_prefix(new LogCache(cold_capacity, capacity, cache_block_size, _cache_trace, trace_file, 
            cold_trace_file, waf_log_file, std::make_unique<CbEvictPolicy>(score_age_evict), 
            nullptr, nullptr, 0.93, std::make_unique<KthCbEvictPolicy>(score_age, ranking), 1.0), cache_type);
    }
    else {
        assert(false);
    }
    return nullptr;
}

ICache::~ICache() {
    if (fp_victim_ratio) { fclose(fp_victim_ratio); fp_victim_ratio = nullptr; }
}

ICache::ICache(uint64_t /*cold_capacity*/, const std::string& waf_log_file,
	       const std::string& input_stat_log_file)
{
	write_size_to_cache = 0;
	evicted_blocks = 0;
	write_hit_size = 0;
	next_write_size_to_cache = TEN_GB;

	std::string waf_path = waf_log_file.empty() ? "waf.log" : waf_log_file;
	fp = fopen(waf_path.c_str(), "w");
	if (fp == nullptr) {
		std::cerr << "Cannot open file: " << waf_path << std::endl;
		exit(1);
	}

	std::string stat_log_file = input_stat_log_file.empty() ? "stat.log." + get_timestamp()
								: input_stat_log_file;
	std::string object_log_file = "object.log." + get_timestamp();

	fp_stats = fopen(stat_log_file.c_str(), "w");
	if (fp_stats == nullptr) {
		std::cerr << "Cannot open file: " << stat_log_file << std::endl << std::endl;
		exit(1);
	}
	fp_object = fopen(object_log_file.c_str(), "w");
	if (fp_object == nullptr) {
		std::cerr << "Cannot open file: " << object_log_file << std::endl;
		exit(1);
	}

	printf("waf log : %s, stat log : %s, object log : %s\n",
	       waf_path.c_str(), stat_log_file.c_str(), object_log_file.c_str());
	assert(fp != nullptr);
	assert(fp_stats != nullptr);
	assert(fp_object != nullptr);
}

void ICache::set_stats_prefix(const std::string& prefix) {
    stats_prefix_ = prefix;
    // Open victim_ratio CSV with policy name + timestamp
    if (!fp_victim_ratio) {
        std::string path = "victim_ratio_" + prefix + "_" + get_timestamp() + ".csv";
        fp_victim_ratio = fopen(path.c_str(), "w");
        if (fp_victim_ratio) {
            fprintf(fp_victim_ratio, "timestamp,type,class,valid_blocks,total_blocks,valid_ratio\n");
            fflush(fp_victim_ratio);
            printf("victim ratio log : %s\n", path.c_str());
        }
    }
}

const std::string& ICache::stats_prefix() const {
    return stats_prefix_;
}

void ICache::_evict_one_block(uint64_t /*lba_offset*/, int lba_size, OP_TYPE op_type)
{
	if (op_type != OP_TYPE::WRITE) {
		return;
	}
	(void)lba_size;
	if (write_size_to_cache > next_write_size_to_cache) {
		next_write_size_to_cache += TEN_GB;
		if (fp) {
			fprintf(fp, "%lld %lld %d %d\n", write_size_to_cache,
				evicted_blocks * get_block_size(), 0, 0);
			fflush(fp);
		}
	}
}

void ICache::_invalidate_cold_block(uint64_t, int, OP_TYPE)
{
}

std::tuple<long long, long long, long long> ICache::get_status() {
    
    return std::tuple<long long, long long, long long>(write_size_to_cache, evicted_blocks, write_hit_size);
}
