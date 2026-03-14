#pragma once
#include "icache.h"              // 기존 프로젝트 공용 인터페이스
#include "log_cache_segment.h"
#include "evict_policy.h"
#include "evict_policy_greedy.h"
#include "istream.h"
#include "histogram.h"
#include "emwa_ratio.h"
#include "ghost_cache.h"
#include "cache_device.h"

#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <list>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <utility>

#if __cplusplus < 201402L
namespace std {
template <typename T, typename... Args>
inline unique_ptr<T> make_unique(Args &&... args)
{
	return unique_ptr<T>(new T(std::forward<Args>(args)...));
}
}
#endif

/**
 * Multi‑stream append‑only Log Cache (세그먼트 단위)
 */
struct Config
{
    std::size_t segment_bytes  = 512ull * 1024 * 1024; ///< default 512 MB zone
    std::size_t zone_size_bytes = 0;                   ///< zone size for physical_base (0 = use segment_bytes)
    std::size_t zone_capacity_bytes = 0;               ///< writable capacity per zone (for striping)
    int         stripe_width = STRIPE_WIDTH;           ///< number of zones per segment

    double      free_ratio_low = 0.01;                ///< 1 %
    int         evicted_blk_size = 1;    // 4k eviction
    uint64_t         print_stats_interval = 10 * 1024ull * 1024 * 1024; // 10 GB
};
// Incremental GC/Evict: process 1GB segment in 16MB chunks
// Set to false to bypass and process entire segment at once
static constexpr bool INCREMENTAL_GC_ENABLED = false;

// Free segment thresholds for GC triggering
static constexpr size_t CRITICAL_FREE_SEGMENTS = 2;   // Block host IO if <= this
static constexpr size_t LOW_FREE_SEGMENTS = 10;       // Trigger GC if <= this
#define GHOST_CACHE 1
#define NETFREE_TCO_ENABLED 0

class LogCache final : public ICache
{
public:
    struct BlockPayload {
        long key;
        int lba_size;
        const void *data;
    };

    struct BlockReadRequest {
        long key;
        void *data;
        size_t len;
    };

    // Async GC/Evict helper structures
    struct GcBlockInfo {
        uint64_t src_offset;      // Source offset in cache device
        uint64_t dst_offset;      // Destination offset in new segment
        long key;
        size_t src_idx;           // Source index in victim segment
        size_t dst_idx;           // Destination index in target segment (for striping)
        uint32_t create_timestamp;
        uint32_t gc_copied_timestamp; // GC 최초 복사 시각 (0이면 이번이 첫 GC copy)
        LogCacheSegment *dst_seg; // Target segment for this block (may differ from result.target_seg when segment fills)
    };

    struct GcPrepareResult {
        LogCacheSegment *victim_seg = nullptr;
        LogCacheSegment *target_seg = nullptr;
        std::vector<GcBlockInfo> blocks_to_copy;
        uint64_t threshold = 0;
        int gc_stream_id = 0;
        bool do_evict_only = false;       // true if all blocks should be evicted (no GC copy)
        // Incremental GC support (16MB chunks)
        size_t chunk_start = 0;           // Start of current chunk (for finalize)
        size_t scan_offset = 0;           // End of current chunk / start of next
        bool is_final_chunk = false;      // true if this is the last chunk
        static constexpr size_t CHUNK_BLOCKS = 4096;  // 16MB = 4096 * 4KB
    };

    struct EvictBlockInfo {
        static constexpr size_t CHUNK_BLOCKS = 32;  // 128k = 32 * 4k
        uint64_t cache_chunk_idx;     // 128k chunk index within segment
        std::vector<bool> valid_mask;  // [32] which 4k blocks are valid
        std::vector<uint64_t> backend_keys;  // [32] backend LBA for each block
        std::vector<size_t> seg_indices;  // [32] segment block indices
    };

    struct EvictPrepareResult {
        LogCacheSegment *victim_seg = nullptr;
        std::vector<EvictBlockInfo> chunks;
        // Incremental Evict support (16MB chunks)
        size_t chunk_start = 0;           // Start of current chunk (for finalize)
        size_t scan_offset = 0;           // End of current chunk / start of next
        bool is_final_chunk = false;      // true if this is the last chunk
        static constexpr size_t CHUNK_BLOCKS = 4096;  // 16MB = 4096 * 4KB
    };

    LogCache(uint64_t              cold_capacity,
             uint64_t              cache_block_count,
             int                   cache_block_size,
             bool                  cache_trace,
             const std::string&    trace_file,
             const std::string&    cold_trace,
             std::string&    waf_log_file,
             std::unique_ptr<EvictPolicy> ev =
                 std::make_unique<GreedyEvictPolicy>(),
             const Config*         cfg           = nullptr,
             IStream *input_stream_policy = nullptr,
             double target_valid_blk_rate = 0.0,
             std::unique_ptr<EvictPolicy> compactor = nullptr,
             double max_age_ratio_by_gc = 0.0,
             bool input_ghost_cache = false,
             std::string stat_log_file = "",
             CacheDeviceInterface *device_io = nullptr
            );

    ~LogCache();

    /* ICache overrides */
    bool        exists(long key) override;
    void        touch(long, OP_TYPE) override {}              // no‑op
    std::size_t size() override { return mapping.size(); }

    /* 새로운 API – stream id 포함 */
    void batch_insert(int stream_id, const std::map<long,int>& newBlocks,
                      OP_TYPE                   op_type = OP_TYPE::WRITE);
    void ingest_payload(int stream_id, const std::vector<BlockPayload>& payloads);
    int read_block(long key, void *buf, size_t len);
    int read_blocks(const std::vector<BlockReadRequest>& reqs);
    int get_block_size() override;
    void evict_one_block() override;
    void evict(LogCacheSegment *seg, std::size_t idx);
    bool is_cache_filled() override;
    virtual void print_stats() override;
    void print_histograms(bool reset = false);
    void log_victim_age_dist(const char *label, LogCacheSegment *victim);
    void print_objects(std::string prefix, uint64_t value);
    void invalidate(long key, int lba_sz);
    void reset_segment(LogCacheSegment *seg);
    void reset_segment_async(LogCacheSegment *seg, cache_device_io_cb cb, void *cb_arg);
    void complete_segment_reset(LogCacheSegment *seg);  // Called by async callback
    // Append block metadata only, returns cache offset for async write
    // out_stream_id: optional output for the actual stream_id assigned (for FDP placement handle)
    // out_segment_full: optional output, set to true if segment became full after this write
    // Adds key to pending_writes_; call complete_block_write() after write completes
    bool append_block_metadata(int stream_id, long key, int lba_sz, uint64_t *cache_offset, int *out_stream_id = nullptr, bool *out_segment_full = nullptr);
    // Mark block write as complete (removes from pending_writes_)
    void complete_block_write(long key);
    void complete_block_writes(const std::vector<long>& keys);  // batch version
    void dummy_fill_segment(LogCacheSegment* s);
    void set_device_io(CacheDeviceInterface *io) { device_io_ = io; }
    //void do_evict_and_compaction_with_same_policy();

    // Async mode - when true, check_and_evict_if_needed becomes no-op
    // (GC/Evict handled by async wrapper instead)
    void set_async_mode(bool async) { async_mode_ = async; }
    bool is_async_mode() const { return async_mode_; }

    // For async wrapper access (no-op in async mode)
    void check_and_evict_if_needed();

    // Async GC/Evict helpers
    bool need_gc_or_evict() const;
    bool prepare_gc(GcPrepareResult &result);
    bool prepare_evict(EvictPrepareResult &result);
    void finalize_gc(GcPrepareResult &result);
    void finalize_gc_async(GcPrepareResult &result, cache_device_io_cb cb, void *cb_arg);
    void finalize_evict(EvictPrepareResult &result);
    void finalize_evict_async(EvictPrepareResult &result, cache_device_io_cb cb, void *cb_arg);
    void abort_gc(GcPrepareResult &result);      // Returns victim to evictor when GC fails
    void abort_evict(EvictPrepareResult &result); // Returns victim to evictor when evict fails
    uint64_t block_offset(const LogCacheSegment *seg, std::size_t idx) const;

    // Mapping access for async read
    bool get_cache_location(long key, uint64_t *offset);

    // Free segment count for watermark checks
    size_t free_segment_count() const { return free_pool.size(); }

    // Host write handle for FDP placement (0 or 1, toggled on segment full)
    int get_host_write_handle() const { return host_write_handle_; }
    void toggle_host_write_handle() { host_write_handle_ = 1 - host_write_handle_; }

    // Number of host streams (for dynamic GC PH base offset)
    int getNumHostStreams() const { return stream_policy ? stream_policy->getNumHostStreams() : 2; }

    // Stats getters for StatsLogger
    uint64_t get_valid_blocks() const { return global_valid_blocks; }
    uint64_t get_write_hit_count() const { return write_hit_size; }
    uint64_t get_compacted_blocks() const { return compacted_blocks; }
    uint64_t get_evicted_blocks() const { return evicted_blocks; }
    uint64_t get_gc_segments_allocated() const { return gc_segments_allocated; }

private:
    /* configuration ******************************************************/
    const int         cache_block_size;
    Config            cfg_;

    /* segment pools ******************************************************/
    std::deque<LogCacheSegment*>                free_pool;
    std::list<std::unique_ptr<LogCacheSegment>> all_segments; // owner
    std::unordered_map<int, LogCacheSegment*>   active_seg;   // stream→seg
    std::unordered_map<int, LogCacheSegment*>   gc_active_seg;   // stream→seg

    /* page lookup ********************************************************/
    struct Loc { LogCacheSegment* seg; std::size_t idx; };
    std::unordered_map<long, Loc>                mapping;
    std::unordered_set<long>                     pending_writes_;  // keys with write in progress

    /* helpers ************************************************************/
    std::unique_ptr<EvictPolicy> evictor;
    CacheDeviceInterface *device_io_ = nullptr;
    bool async_mode_ = false;  // When true, sync eviction is disabled
    int host_write_handle_ = 0;  // Toggle between 0 and 1 for host writes (FDP)

    LogCacheSegment* alloc_segment(bool shrink = true);
    void             evict_segment(LogCacheSegment* s);
    Segment*         evict_and_compaction(LogCacheSegment* s, uint64_t threshold, int gc_stream_id = 0);

    void             evict_policy_add(LogCacheSegment *s);
    void             evict_policy_remove(LogCacheSegment *s);
    void             evict_policy_update(LogCacheSegment *s);
    LogCacheSegment* get_segment_to_active_stream(bool gc, int stream, bool check_only = false);
    LogCacheSegment* get_segment_with_stream_policy(bool gc, uint64_t key, bool check_only = false);
    void periodic();

    /* trace(optional) *****************************************************/
    bool  cache_trace_;
    FILE* trace_fp_      = nullptr;
    FILE* cold_trace_fp_ = nullptr;
    std::size_t segment_size_blocks;
    std::size_t total_segments;
    uint64_t total_cache_block_count = 0;

    uint64_t total_capacity_bytes = 0;
    uint64_t log_cache_timestamp = 0; // per 4kB block
    IStream *stream_policy = nullptr;
    uint64_t global_valid_blocks = 0;
    uint64_t compacted_blocks = 0;
    uint64_t gc_segments_allocated = 0;  // cumulative GC segment allocations
    uint64_t gc_freed_blocks = 0;        // net freed blocks from GC (= segment_size - compacted per GC'd segment)
    uint64_t invalidate_blocks = 0;
    uint64_t reinsert_blocks = 0;
    uint64_t read_blocks_in_partial_write = 0;

    uint64_t evicted_segment_age = 0;

    double target_valid_blk_rate = 0.0; // ratio of write to QLC
    double valid_blk_rate_hard_limit = 0.0;
    std::unique_ptr<EvictPolicy> compactor;
    double additional_free_blks_ratio_by_gc;
    
    std::unique_ptr<Histogram> evicted_ages_with_segment_histogram;
    std::unique_ptr<Histogram> compacted_ages_with_segment_histogram;
    std::unique_ptr<Histogram> gc_copied_lifetime_histogram;
    static const int HISTOGRAM_BUCKETS = 40;
    static const uint64_t DEFAULT_HALF_LIFE_IN_BLOCKS = (262144 * 6) * 4;
    static constexpr double GHOST_CACHE_RATIO = 0.1;  // 5% of cache size
    static constexpr double QLC_TLC_COST_RATIO = 2.88 * 3;//(2.88 * 1); // QLC write cost / TLC write cost
    bool is_ghost_cache = false;
    uint64_t bypass_blocks_threshold = 128; // 128* 4k bytes = 512K bytes
    EwmaRatio compaction_ratio;
    EwmaRatio gc_cost_ratio;             // compacted_blocks / gc_freed_blocks = V_g/(1-V_g), cost per freed block from GC
    EwmaRatio eviction_ratio;
    EwmaRatio evict_cost_ratio;          // evicted_blocks / evict_freed_blocks, cost per freed block from eviction
    uint64_t evict_freed_blocks = 0;     // total blocks freed by segment eviction
    EwmaRatio ghost_compaction_ratio;    // ghost compacted / timestamp, per host write
    EwmaRatio ghost_eviction_ratio;      // ghost evicted / timestamp, per host write
    EwmaRatio ghost_reuse_ewma;          // d(accessHit) / d(push), reuse rate of evicted blocks
    uint64_t ghost_compacted_blocks = 0;
    uint64_t ghost_gc_freed_blocks = 0;
#if NETFREE_TCO_ENABLED
    uint64_t gc_victim_count_ = 0;           // cumulative GC victim segments
    uint64_t gc_active_alloc_count_ = 0;     // cumulative GC new segment allocations
    uint64_t cumulative_B_ = 0;              // cumulative get_mth_score_valid_pages sum
    EwmaRatio netfree_a_ratio;               // EWMA of A * seg_size over timestamp
    EwmaRatio netfree_b_ratio;               // EWMA of cumulative_B over timestamp
#endif
    GhostCache ghost_cache;
    std::vector<uint8_t> staging_buffer_;

    void append_block(int stream_id, long key, int lba_sz, const void *payload);
    int write_cache_data(uint64_t offset, const void *buf, size_t len);
    int read_cache_data(uint64_t offset, void *buf, size_t len);
    int write_backend_data(uint64_t offset, const void *buf, size_t len);
    int read_backend_data(uint64_t offset, void *buf, size_t len);
    void reset_cache_region(LogCacheSegment *seg);
    void ensure_staging_buffer(size_t len);
    void copy_block(LogCacheSegment *src_seg, std::size_t src_idx, LogCacheSegment *target_seg);
    void flush_block_to_backend(LogCacheSegment *seg, std::size_t idx);
};
