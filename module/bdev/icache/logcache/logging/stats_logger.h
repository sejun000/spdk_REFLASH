#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <cstdio>
#include <functional>

extern "C" {
#include "spdk/thread.h"
#include "spdk/nvme.h"
#include "spdk/bdev.h"
}

/**
 * NVMe FDP Statistics Log Page (Log ID 0x20)
 * Used to read HBMW (Host Bytes with Metadata Written) and MBMW (Media Bytes with Metadata Written)
 * for WAF calculation on FDP-enabled SSDs.
 *
 * Read with: nvme fdp stats /dev/nvmeX -e <endurance_group_id>
 */
#pragma pack(push, 1)
struct NvmeFdpStatsLog {
    uint8_t hbmw[16];    // Host Bytes with Metadata Written (128-bit, in bytes)
    uint8_t mbmw[16];    // Media Bytes with Metadata Written (128-bit, in bytes)
    uint8_t mbe[16];     // Media Bytes Erased (128-bit, in bytes)
    uint8_t rsvd48[16];  // Reserved
};  // Total: 64 bytes (matches nvme-cli/libnvme)
#pragma pack(pop)

/**
 * Backend vendor-specific log page 0xC0
 * Bytes 24-31: NAND writes (in SMART units, 1 unit = 512,000 bytes)
 */
#pragma pack(push, 1)
struct VendorLogPage0xC0 {
    uint8_t data[256];   // read enough to cover offset 24-31
};
#pragma pack(pop)

/**
 * Stats Logger for icache
 *
 * Periodically logs write statistics to a file:
 * - Host Write Request: bytes received from host
 * - Cache Write: bytes written to cache device
 * - Backend Write: bytes written to backend device
 * - GC Write: bytes written by GC operations
 * - Host Written Diff: NVMe data_units_written delta (from log page)
 * - Media Written Diff: NVMe media_units_written delta (from log page)
 *
 * Log file location: logging/<policy_name>_<timestamp>.csv
 */
class StatsLogger {
public:
    // Stats counters (updated by icache operations)
    struct Stats {
        std::atomic<uint64_t> host_write_bytes{0};    // Host write requests
        std::atomic<uint64_t> cache_write_bytes{0};   // Writes to cache device
        std::atomic<uint64_t> backend_write_bytes{0}; // Writes to backend device
        std::atomic<uint64_t> gc_write_bytes{0};      // GC copy writes
        std::atomic<uint64_t> cache_read_bytes{0};    // Reads from cache device
        std::atomic<uint64_t> backend_read_bytes{0};  // Reads from backend device
        std::atomic<uint64_t> valid_blocks{0};        // Valid 4K blocks in cache
        std::atomic<uint64_t> write_hit_count{0};     // Write cache hits (same LBA invalidated)
        std::atomic<uint64_t> gc_victim_blocks{0};    // Blocks copied by GC (compaction)
        std::atomic<uint64_t> evict_victim_blocks{0}; // Blocks evicted to backend
        std::atomic<uint64_t> gc_count{0};            // Number of completed GC operations
        std::atomic<uint64_t> evict_count{0};         // Number of completed evict operations
        std::atomic<uint64_t> flush_count{0};         // Number of flush_write_buffer calls
        std::atomic<uint64_t> gc_segments_allocated{0}; // Cumulative GC segment allocations
		std::atomic<uint64_t> backend_trim_bytes{0};  // Successfully deallocated backend bytes
		std::atomic<uint64_t> backend_trim_enabled{0}; // 1 while DSM batching is active
		std::atomic<uint64_t> backend_trim_commands{0}; // Successfully completed DSM commands
		std::atomic<uint64_t> backend_trim_ranges{0}; // Successfully completed DSM ranges
		std::atomic<uint64_t> backend_trim_errors{0}; // Build, submit, and completion errors
		std::atomic<uint64_t> backend_trim_outstanding{0}; // DSM commands currently in flight
		std::atomic<uint64_t> backend_trim_pending_batches{0}; // Full 256-key batches waiting
    };

    /**
     * Create stats logger
     * @param policy_name Policy name for log file (e.g., "LOG_GREEDY")
     * @param log_dir Directory for log files (created if not exists)
     * @param interval_us Logging interval in microseconds (default: 2 seconds)
     */
    StatsLogger(const std::string& policy_name,
                const std::string& log_dir = "logging",
                uint64_t interval_us = 2000000);

    ~StatsLogger();

    // Set NVMe controller for log page reading (can be called after start)
    void set_nvme_ctrlr(struct spdk_nvme_ctrlr *ctrlr);

    // Set backend NVMe controller for NAND writes reading (vendor log page 0xC0)
    void set_backend_nvme_ctrlr(struct spdk_nvme_ctrlr *ctrlr);

    // Initialize and start the logger (call from SPDK thread)
    bool start();

    // Stop the logger
    void stop();

    // Get stats reference for updating
    Stats& stats() { return stats_; }

    // Add to counters (thread-safe)
    void add_host_write(uint64_t bytes) { stats_.host_write_bytes.fetch_add(bytes, std::memory_order_relaxed); }
    void add_cache_write(uint64_t bytes) { stats_.cache_write_bytes.fetch_add(bytes, std::memory_order_relaxed); }
    void add_backend_write(uint64_t bytes) { stats_.backend_write_bytes.fetch_add(bytes, std::memory_order_relaxed); }
    void add_gc_write(uint64_t bytes) { stats_.gc_write_bytes.fetch_add(bytes, std::memory_order_relaxed); }
    void add_cache_read(uint64_t bytes) { stats_.cache_read_bytes.fetch_add(bytes, std::memory_order_relaxed); }
    void add_backend_read(uint64_t bytes) { stats_.backend_read_bytes.fetch_add(bytes, std::memory_order_relaxed); }
    void set_valid_blocks(uint64_t count) { stats_.valid_blocks.store(count, std::memory_order_relaxed); }
    void add_write_hit() { stats_.write_hit_count.fetch_add(1, std::memory_order_relaxed); }
    void add_gc_victim_blocks(uint64_t count) { stats_.gc_victim_blocks.fetch_add(count, std::memory_order_relaxed); }
    void add_evict_victim_blocks(uint64_t count) { stats_.evict_victim_blocks.fetch_add(count, std::memory_order_relaxed); }
    void inc_gc_count() { stats_.gc_count.fetch_add(1, std::memory_order_relaxed); }
    void inc_evict_count() { stats_.evict_count.fetch_add(1, std::memory_order_relaxed); }
    void inc_flush_count() { stats_.flush_count.fetch_add(1, std::memory_order_relaxed); }
    void set_gc_segments_allocated(uint64_t count) { stats_.gc_segments_allocated.store(count, std::memory_order_relaxed); }
	void set_backend_trim_stats(bool enabled, uint64_t bytes, uint64_t commands, uint64_t ranges,
				    uint64_t errors, uint64_t outstanding, uint64_t pending_batches) {
		stats_.backend_trim_enabled.store(enabled ? 1 : 0, std::memory_order_relaxed);
		stats_.backend_trim_bytes.store(bytes, std::memory_order_relaxed);
		stats_.backend_trim_commands.store(commands, std::memory_order_relaxed);
		stats_.backend_trim_ranges.store(ranges, std::memory_order_relaxed);
		stats_.backend_trim_errors.store(errors, std::memory_order_relaxed);
		stats_.backend_trim_outstanding.store(outstanding, std::memory_order_relaxed);
		stats_.backend_trim_pending_batches.store(pending_batches, std::memory_order_relaxed);
	}

    // Set histogram print callback (called every ~60 seconds)
    void set_histogram_callback(std::function<void()> cb) { histogram_cb_ = std::move(cb); }

    // Get current values
    uint64_t host_write_bytes() const { return stats_.host_write_bytes.load(std::memory_order_relaxed); }
    uint64_t cache_write_bytes() const { return stats_.cache_write_bytes.load(std::memory_order_relaxed); }
    uint64_t backend_write_bytes() const { return stats_.backend_write_bytes.load(std::memory_order_relaxed); }
    uint64_t gc_write_bytes() const { return stats_.gc_write_bytes.load(std::memory_order_relaxed); }

    // WAF values (computed from log page deltas)
    double fdp_waf() const { return fdp_waf_; }        // cache SSD: MBMW/HBMW
    double backend_waf() const { return backend_waf_; } // backend SSD: nand/host

private:
    // SPDK poller callback
    static int poller_fn(void *arg);

    // NVMe log page completion callback
    static void log_page_done(void *cb_arg, const struct spdk_nvme_cpl *cpl);

    // Write one log line
    void log_stats();

    // Read NVMe endurance group log page
    void read_nvme_log_page();

    // Read backend vendor log page 0xC0 for NAND writes
    void read_backend_log_page();
    static void backend_log_page_done(void *cb_arg, const struct spdk_nvme_cpl *cpl);

    std::string policy_name_;
    std::string log_dir_;
    std::string log_path_;
    uint64_t interval_us_;

    FILE *log_fp_ = nullptr;
    struct spdk_poller *poller_ = nullptr;
    bool started_ = false;

    Stats stats_;

    // Previous values for delta calculation
    uint64_t prev_host_write_ = 0;
    uint64_t prev_cache_write_ = 0;
    uint64_t prev_backend_write_ = 0;
    uint64_t prev_gc_write_ = 0;
    uint64_t prev_cache_read_ = 0;
    uint64_t prev_backend_read_ = 0;

    uint64_t start_time_us_ = 0;

    // NVMe log page reading
    struct spdk_nvme_ctrlr *nvme_ctrlr_ = nullptr;
    NvmeFdpStatsLog *log_page_buf_ = nullptr;
    bool log_page_pending_ = false;

    // NVMe FDP stats (from log page, in bytes)
    uint64_t nvme_host_written_ = 0;     // HBMW: Host Bytes with Metadata Written
    uint64_t nvme_media_written_ = 0;    // MBMW: Media Bytes with Metadata Written
    uint64_t prev_nvme_host_written_ = 0;
    uint64_t prev_nvme_media_written_ = 0;

    // WAF computation: cumulative average from the point where both writes became non-zero
    double fdp_waf_ = 1.0;          // cache SSD WAF: (MBMW-start)/(HBMW-start)
    double backend_waf_ = 1.0;      // backend SSD WAF: (nand-start)/(host-start)
    uint64_t waf_start_nvme_host_ = 0;
    uint64_t waf_start_nvme_media_ = 0;
    bool     waf_fdp_started_ = false;
    uint64_t waf_start_backend_nand_ = 0;
    uint64_t waf_start_backend_write_ = 0;
    bool     waf_backend_started_ = false;

    // Backend NVMe controller for vendor log page 0xC0
    struct spdk_nvme_ctrlr *backend_nvme_ctrlr_ = nullptr;
    VendorLogPage0xC0 *backend_log_page_buf_ = nullptr;
    bool backend_log_page_pending_ = false;
    uint64_t backend_nand_written_ = 0;         // NAND writes in bytes (value * 512000)
    uint64_t prev_backend_nand_written_ = 0;

    // Histogram periodic print (every ~60 seconds)
    std::function<void()> histogram_cb_;
    uint64_t histogram_interval_us_ = 60000000;  // 60 seconds
    uint64_t last_histogram_us_ = 0;
};
