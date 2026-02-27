#include "stats_logger.h"

#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <cstring>

extern "C" {
#include "spdk/log.h"
#include "spdk/env.h"
}

// NVMe Log Page ID for FDP Statistics (from NVMe spec)
static constexpr uint8_t NVME_LOG_LID_FDP_STATS = 0x22;

StatsLogger::StatsLogger(const std::string& policy_name,
                         const std::string& log_dir,
                         uint64_t interval_us)
    : policy_name_(policy_name),
      log_dir_(log_dir),
      interval_us_(interval_us)
{
}

StatsLogger::~StatsLogger()
{
    stop();
    if (log_page_buf_) {
        spdk_dma_free(log_page_buf_);
        log_page_buf_ = nullptr;
    }
}

void StatsLogger::set_nvme_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
    nvme_ctrlr_ = ctrlr;
    SPDK_NOTICELOG("StatsLogger: set_nvme_ctrlr called with ctrlr=%p\n", ctrlr);

    // Allocate DMA buffer for NVMe FDP stats log page if not already allocated
    if (ctrlr && !log_page_buf_) {
        log_page_buf_ = static_cast<NvmeFdpStatsLog*>(
            spdk_dma_zmalloc(sizeof(NvmeFdpStatsLog), 4096, nullptr));
        if (log_page_buf_) {
            SPDK_NOTICELOG("StatsLogger: allocated FDP stats log page buffer (%zu bytes)\n",
                           sizeof(NvmeFdpStatsLog));
        } else {
            SPDK_WARNLOG("StatsLogger: failed to allocate log page buffer, NVMe FDP stats disabled\n");
        }
    }
}

bool StatsLogger::start()
{
    if (started_) {
        return true;
    }

    // Create log directory if not exists
    struct stat st;
    if (stat(log_dir_.c_str(), &st) != 0) {
        if (mkdir(log_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
            SPDK_ERRLOG("StatsLogger: failed to create directory %s: %s\n",
                        log_dir_.c_str(), strerror(errno));
            return false;
        }
    }

    // Generate log filename with timestamp
    time_t now = time(nullptr);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

    log_path_ = log_dir_ + "/" + policy_name_ + "_" + timestamp + ".csv";

    // Open log file
    log_fp_ = fopen(log_path_.c_str(), "w");
    if (!log_fp_) {
        SPDK_ERRLOG("StatsLogger: failed to open log file %s: %s\n",
                    log_path_.c_str(), strerror(errno));
        return false;
    }

    // Allocate DMA buffer for NVMe FDP stats log page
    if (nvme_ctrlr_ && !log_page_buf_) {
        log_page_buf_ = static_cast<NvmeFdpStatsLog*>(
            spdk_dma_zmalloc(sizeof(NvmeFdpStatsLog), 4096, nullptr));
        if (!log_page_buf_) {
            SPDK_WARNLOG("StatsLogger: failed to allocate log page buffer, NVMe FDP stats disabled\n");
        }
    }

    // Write CSV header
    fprintf(log_fp_, "time_sec,host_write_MB,cache_write_MB,backend_write_MB,gc_write_MB,"
                     "host_write_delta_MB,cache_write_delta_MB,backend_write_delta_MB,gc_write_delta_MB,"
                     "nvme_host_written_MB,nvme_media_written_MB,nvme_host_delta_MB,nvme_media_delta_MB,"
                     "cache_read_MB,backend_read_MB,valid_blocks,write_hit_count,gc_victim_blocks,evict_victim_blocks,"
                     "gc_count,evict_count,flush_count,gc_segments_allocated\n");
    fflush(log_fp_);

    // Record start time
    start_time_us_ = spdk_get_ticks() * 1000000 / spdk_get_ticks_hz();

    // Register SPDK poller
    poller_ = spdk_poller_register(poller_fn, this, interval_us_);
    if (!poller_) {
        SPDK_ERRLOG("StatsLogger: failed to register poller\n");
        fclose(log_fp_);
        log_fp_ = nullptr;
        return false;
    }

    started_ = true;
    SPDK_NOTICELOG("StatsLogger: started logging to %s (interval=%lu us, nvme_ctrlr=%p)\n",
                   log_path_.c_str(), interval_us_, nvme_ctrlr_);
    return true;
}

void StatsLogger::stop()
{
    if (!started_) {
        return;
    }

    if (poller_) {
        spdk_poller_unregister(&poller_);
        poller_ = nullptr;
    }

    // Write final stats
    if (log_fp_) {
        log_stats();
        fclose(log_fp_);
        log_fp_ = nullptr;
    }

    started_ = false;
    SPDK_NOTICELOG("StatsLogger: stopped\n");
}

int StatsLogger::poller_fn(void *arg)
{
    auto *self = static_cast<StatsLogger*>(arg);

    // Process admin completions to receive FDP log page results
    if (self->nvme_ctrlr_) {
        spdk_nvme_ctrlr_process_admin_completions(self->nvme_ctrlr_);
    }

    // Try to read NVMe log page (async)
    self->read_nvme_log_page();

    // Log current stats
    self->log_stats();

    // Check if histogram print is due (~60 seconds)
    if (self->histogram_cb_) {
        uint64_t now_us = spdk_get_ticks() * 1000000 / spdk_get_ticks_hz();
        if (now_us - self->last_histogram_us_ >= self->histogram_interval_us_) {
            self->histogram_cb_();
            self->last_histogram_us_ = now_us;
        }
    }

    return SPDK_POLLER_BUSY;
}

void StatsLogger::log_page_done(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    auto *self = static_cast<StatsLogger*>(cb_arg);
    self->log_page_pending_ = false;

    if (spdk_nvme_cpl_is_error(cpl)) {
        SPDK_WARNLOG("StatsLogger: NVMe FDP stats log page read failed, sct=%d sc=%d\n",
                     cpl->status.sct, cpl->status.sc);
        return;
    }

    // Successfully read FDP stats log page - extract values
    // HBMW and MBMW are 128-bit little-endian values in bytes
    // We only need the lower 64 bits (up to 16 exabytes is enough)
    uint64_t hbmw_lo, mbmw_lo;
    memcpy(&hbmw_lo, self->log_page_buf_->hbmw, sizeof(uint64_t));
    memcpy(&mbmw_lo, self->log_page_buf_->mbmw, sizeof(uint64_t));
    self->nvme_host_written_ = hbmw_lo;
    self->nvme_media_written_ = mbmw_lo;
}

void StatsLogger::read_nvme_log_page()
{
    if (!nvme_ctrlr_ || !log_page_buf_ || log_page_pending_) {
        return;
    }

    // Read FDP Statistics Log Page (LID=0x22)
    // Based on nvme-cli implementation:
    // - CDW11 bits 31:16 = LSI (Log Specific Identifier) = Endurance Group ID
    // - CDW14 bits 31:24 = CSI (Command Set Identifier) = 0 (NVM)
    // - nsid = 0
    uint16_t egid = 1;  // Endurance Group ID
    uint32_t cdw11 = (uint32_t)egid << 16;  // LSI in bits 31:16
    uint32_t cdw14 = 0;  // CSI=0 (NVM) in bits 31:24

    int rc = spdk_nvme_ctrlr_cmd_get_log_page_ext(
        nvme_ctrlr_,
        NVME_LOG_LID_FDP_STATS,
        0,  // nsid = 0
        log_page_buf_,
        sizeof(NvmeFdpStatsLog),
        0,      // offset
        0,      // cdw10 (handled internally)
        cdw11,  // LSI (Endurance Group ID) in bits 31:16
        cdw14,  // CSI in bits 31:24
        log_page_done,
        this);

    if (rc == 0) {
        log_page_pending_ = true;
    }
}

void StatsLogger::log_stats()
{
    if (!log_fp_) {
        return;
    }

    // Get current values
    uint64_t host_write = stats_.host_write_bytes.load(std::memory_order_relaxed);
    uint64_t cache_write = stats_.cache_write_bytes.load(std::memory_order_relaxed);
    uint64_t backend_write = stats_.backend_write_bytes.load(std::memory_order_relaxed);
    uint64_t gc_write = stats_.gc_write_bytes.load(std::memory_order_relaxed);
    uint64_t cache_read = stats_.cache_read_bytes.load(std::memory_order_relaxed);
    uint64_t backend_read = stats_.backend_read_bytes.load(std::memory_order_relaxed);
    uint64_t valid_blocks = stats_.valid_blocks.load(std::memory_order_relaxed);
    uint64_t write_hit_count = stats_.write_hit_count.load(std::memory_order_relaxed);
    uint64_t gc_victim_blocks = stats_.gc_victim_blocks.load(std::memory_order_relaxed);
    uint64_t evict_victim_blocks = stats_.evict_victim_blocks.load(std::memory_order_relaxed);
    uint64_t gc_count = stats_.gc_count.load(std::memory_order_relaxed);
    uint64_t evict_count = stats_.evict_count.load(std::memory_order_relaxed);
    uint64_t flush_count = stats_.flush_count.load(std::memory_order_relaxed);
    uint64_t gc_segments_allocated = stats_.gc_segments_allocated.load(std::memory_order_relaxed);

    // Calculate deltas
    uint64_t host_delta = host_write - prev_host_write_;
    uint64_t cache_delta = cache_write - prev_cache_write_;
    uint64_t backend_delta = backend_write - prev_backend_write_;
    uint64_t gc_delta = gc_write - prev_gc_write_;

    // NVMe FDP deltas (already in bytes from FDP Statistics Log)
    uint64_t nvme_host_delta = nvme_host_written_ - prev_nvme_host_written_;
    uint64_t nvme_media_delta = nvme_media_written_ - prev_nvme_media_written_;

    // Update previous values
    prev_host_write_ = host_write;
    prev_cache_write_ = cache_write;
    prev_backend_write_ = backend_write;
    prev_gc_write_ = gc_write;
    prev_cache_read_ = cache_read;
    prev_backend_read_ = backend_read;
    prev_nvme_host_written_ = nvme_host_written_;
    prev_nvme_media_written_ = nvme_media_written_;

    // Calculate elapsed time
    uint64_t now_us = spdk_get_ticks() * 1000000 / spdk_get_ticks_hz();
    double elapsed_sec = (now_us - start_time_us_) / 1000000.0;

    // Convert to MB for readability
    constexpr double MB = 1024.0 * 1024.0;

    // Write log line
    // FDP stats (hbmw, mbmw) are already in bytes, just convert to MB
    fprintf(log_fp_, "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
            elapsed_sec,
            host_write / MB,
            cache_write / MB,
            backend_write / MB,
            gc_write / MB,
            host_delta / MB,
            cache_delta / MB,
            backend_delta / MB,
            gc_delta / MB,
            nvme_host_written_ / MB,
            nvme_media_written_ / MB,
            nvme_host_delta / MB,
            nvme_media_delta / MB,
            cache_read / MB,
            backend_read / MB,
            valid_blocks,
            write_hit_count,
            gc_victim_blocks,
            evict_victim_blocks,
            gc_count,
            evict_count,
            flush_count,
            gc_segments_allocated);
    fflush(log_fp_);
}
