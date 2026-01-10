#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <cstdio>

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

    // Get current values
    uint64_t host_write_bytes() const { return stats_.host_write_bytes.load(std::memory_order_relaxed); }
    uint64_t cache_write_bytes() const { return stats_.cache_write_bytes.load(std::memory_order_relaxed); }
    uint64_t backend_write_bytes() const { return stats_.backend_write_bytes.load(std::memory_order_relaxed); }
    uint64_t gc_write_bytes() const { return stats_.gc_write_bytes.load(std::memory_order_relaxed); }

private:
    // SPDK poller callback
    static int poller_fn(void *arg);

    // NVMe log page completion callback
    static void log_page_done(void *cb_arg, const struct spdk_nvme_cpl *cpl);

    // Write one log line
    void log_stats();

    // Read NVMe endurance group log page
    void read_nvme_log_page();

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
};
