/**
 * High-performance Trace Replay
 *
 * Features:
 * - Default: passthrough LBA (trace LBA used as-is, modulo device size)
 * - Optional: sequential LBA remapping with --remap-lba
 * - Auto device detection (ublk > nvmeof > opencas)
 * - Multi-threaded with libaio for maximum performance
 *
 * Build:
 *   g++ -O3 -o replay_trace replay_trace.cpp -laio -pthread
 *
 * Usage:
 *   ./replay_trace [options]
 *   ./replay_trace --trace /path/to/trace --max-tb 4
 *   ./replay_trace --remap-lba --trace /path/to/trace --max-tb 4
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <libaio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <string>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <signal.h>

// Configuration
constexpr int NUM_THREADS = 4;
constexpr int QUEUE_DEPTH = 64;
constexpr int IO_BLOCK_SIZE = 4096;
constexpr int MAX_IO_SIZE = 1024 * 1024;  // 1MB
constexpr uint64_t MAX_PENDING_BYTES = 1 * 1024 * 1024;  // 1MB total pending limit
constexpr int REPORT_INTERVAL = 100000;
constexpr double DEFAULT_MAX_TB = 10.0;
constexpr const char* DEFAULT_TRACE = "/home/sejun000/alibaba_dwpd1.trace.head30p";

// Global state
std::atomic<bool> g_stop_flag{false};
std::atomic<uint64_t> g_max_write_bytes{0};
std::atomic<uint64_t> g_pending_bytes{0};  // Total in-flight bytes across all threads

void signal_handler(int sig) {
    printf("\n[Ctrl+C] Exiting...\n");
    g_stop_flag.store(true);
}

// Thread-safe statistics
class ReplayStats {
public:
    void add(uint64_t ios, uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (start_time_ == std::chrono::steady_clock::time_point{}) {
            start_time_ = std::chrono::steady_clock::now();
            last_report_time_ = start_time_;
        }
        total_ios_ += ios;
        total_bytes_ += bytes;

        // Check write limit
        if (total_bytes_ >= g_max_write_bytes.load()) {
            print_final_unlocked();
            printf("\n[Limit] %.1f TB reached, exiting...\n",
                   g_max_write_bytes.load() / (1024.0*1024*1024*1024));
            exit(0);
        }
    }

    void report(bool force = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (start_time_ == std::chrono::steady_clock::time_point{}) return;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        if (elapsed < 0.001) return;

        uint64_t ios_since_last = total_ios_ - last_report_ios_;
        if (ios_since_last >= REPORT_INTERVAL || force) {
            // All metrics are cumulative averages
            double avg_iops = total_ios_ / elapsed;
            double avg_throughput_mb = (total_bytes_ / (1024.0*1024)) / elapsed;
            double throughput_gb = total_bytes_ / (1024.0*1024*1024);
            printf("[%7.1fs] IOs: %12lu  IOPS: %8.0f  Throughput: %7.1f MB/s  Total: %7.2f GB\n",
                   elapsed, total_ios_, avg_iops, avg_throughput_mb, throughput_gb);
            last_report_ios_ = total_ios_;
            last_report_time_ = now;
        }
    }

    void final_report() {
        std::lock_guard<std::mutex> lock(mutex_);
        print_final_unlocked();
    }

private:
    void print_final_unlocked() {
        if (start_time_ == std::chrono::steady_clock::time_point{}) {
            printf("No IOs completed\n");
            return;
        }
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        if (elapsed < 0.001) elapsed = 0.001;

        double iops = total_ios_ / elapsed;
        double throughput_mb = (total_bytes_ / (1024.0*1024)) / elapsed;
        double throughput_gb = total_bytes_ / (1024.0*1024*1024);
        double throughput_tb = total_bytes_ / (1024.0*1024*1024*1024);

        printf("\n============================================================\n");
        printf("  Final Results (Saturated Replay)\n");
        printf("============================================================\n");
        printf("  Total IOs:       %lu\n", total_ios_);
        printf("  Total Data:      %.2f GB (%.4f TB)\n", throughput_gb, throughput_tb);
        printf("  Elapsed Time:    %.2f seconds\n", elapsed);
        printf("  Average IOPS:    %.0f\n", iops);
        printf("  Average BW:      %.1f MB/s\n", throughput_mb);
        printf("============================================================\n");
    }

    std::mutex mutex_;
    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point last_report_time_{};
    uint64_t total_ios_ = 0;
    uint64_t total_bytes_ = 0;
    uint64_t last_report_ios_ = 0;
    uint64_t last_report_bytes_ = 0;
};

// LBA Mapper: maps trace LBAs to device LBAs
// Default mode: passthrough (use trace LBA as-is, modulo device size)
// Remap mode (--remap-lba): sequential remapping (new LBA -> 0, 1, 2, ...)
class LbaMapper {
public:
    LbaMapper(uint64_t device_size_bytes, bool remap)
        : max_lba_(device_size_bytes / IO_BLOCK_SIZE),
          device_size_bytes_(device_size_bytes),
          remap_(remap) {}

    // Returns mapped device offset for a single 4KB LBA, or -1 if device is full
    int64_t map(uint64_t trace_offset) {
        if (!remap_) {
            // Passthrough: use trace offset directly, wrap around device size
            uint64_t device_offset = trace_offset % device_size_bytes_;
            // Align to IO_BLOCK_SIZE
            device_offset = (device_offset / IO_BLOCK_SIZE) * IO_BLOCK_SIZE;
            total_ios_++;
            return (int64_t)device_offset;
        }

        // Remap mode: sequential LBA assignment
        std::lock_guard<std::mutex> lock(mutex_);

        uint64_t trace_lba = trace_offset / IO_BLOCK_SIZE;

        auto it = lba_map_.find(trace_lba);
        if (it != lba_map_.end()) {
            // Already mapped
            return it->second * IO_BLOCK_SIZE;
        }

        // New LBA - assign next sequential device LBA
        if (next_device_lba_ >= max_lba_) {
            // Device full
            skipped_ios_++;
            skipped_bytes_ += IO_BLOCK_SIZE;
            return -1;
        }

        uint64_t device_lba = next_device_lba_++;
        lba_map_[trace_lba] = device_lba;
        return device_lba * IO_BLOCK_SIZE;
    }

    void print_stats() {
        if (!remap_) {
            printf("\n  LBA Mode: Passthrough (trace LBA used as-is)\n");
            printf("    Total IOs: %lu\n", total_ios_.load());
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t unique_bytes = lba_map_.size() * IO_BLOCK_SIZE;
        printf("\n  LBA Mode: Remap (sequential)\n");
        printf("    Unique LBAs mapped: %lu (%.2f GB)\n", lba_map_.size(), unique_bytes / (1024.0*1024*1024));
        printf("    Skipped IOs:        %lu\n", skipped_ios_);
        printf("    Skipped bytes:      %.2f GB\n", skipped_bytes_ / (1024.0*1024*1024));
    }

    uint64_t get_skipped_ios() const { return skipped_ios_; }
    uint64_t get_unique_lbas() const { return remap_ ? next_device_lba_ : 0; }
    uint64_t get_unique_bytes() const { return remap_ ? next_device_lba_ * IO_BLOCK_SIZE : 0; }
    bool is_remap() const { return remap_; }

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, uint64_t> lba_map_;
    uint64_t max_lba_;
    uint64_t device_size_bytes_;
    bool remap_;
    uint64_t next_device_lba_ = 0;
    uint64_t skipped_ios_ = 0;
    uint64_t skipped_bytes_ = 0;
    std::atomic<uint64_t> total_ios_{0};
};

// Work item
struct WorkItem {
    int64_t offset;  // -1 means end
    uint32_t size;
};

// Thread-safe work queue
class WorkQueue {
public:
    WorkQueue(size_t max_size) : max_size_(max_size) {}

    void push(const WorkItem& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_full_.wait(lock, [this] { return queue_.size() < max_size_ || done_; });
        if (done_) return;
        queue_.push(item);
        cv_not_empty_.notify_one();
    }

    bool pop(WorkItem& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_empty_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) return false;
        item = queue_.front();
        queue_.pop();
        cv_not_full_.notify_one();
        return true;
    }

    void set_done() {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::queue<WorkItem> queue_;
    size_t max_size_;
    bool done_ = false;
};

// Device detection
std::string find_device() {
    // 1. Check ublk device
    const char* ublk_dev = "/dev/ublkb0";
    struct stat st;
    if (stat(ublk_dev, &st) == 0 && S_ISBLK(st.st_mode)) {
        int fd = open(ublk_dev, O_RDONLY);
        if (fd >= 0) {
            uint64_t size = 0;
            if (ioctl(fd, BLKGETSIZE64, &size) == 0 && size > 0) {
                close(fd);
                printf("Auto-detect: ublk device found\n");
                return ublk_dev;
            }
            close(fd);
        }
    }

    // 2. Check NVMe-oF device (look for ICACHE/FTLBDEV/NULL/OCF in model)
    FILE* fp = popen("nvme list 2>/dev/null | grep -iE 'ICACHE|FTLBDEV|NULL|OCF' | awk '{print $1}'", "r");
    if (fp) {
        char buf[256];
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            // Remove newline
            char* nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            if (strlen(buf) > 0) {
                printf("Auto-detect: NVMe-oF device found\n");
                return std::string(buf);
            }
        } else {
            pclose(fp);
        }
    }

    // 3. Check OpenCAS device
    const char* cas_dev = "/dev/cas1-1";
    if (stat(cas_dev, &st) == 0 && S_ISBLK(st.st_mode)) {
        int fd = open(cas_dev, O_RDONLY);
        if (fd >= 0) {
            uint64_t size = 0;
            if (ioctl(fd, BLKGETSIZE64, &size) == 0 && size > 0) {
                close(fd);
                printf("Auto-detect: OpenCAS device found\n");
                return cas_dev;
            }
            close(fd);
        }
    }

    return "";
}

uint64_t get_device_size(const std::string& device) {
    int fd = open(device.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("Failed to open device");
        return 0;
    }

    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
        perror("Failed to get device size");
        close(fd);
        return 0;
    }
    close(fd);
    return size;
}

// Worker thread using libaio
void worker_thread(int thread_id, const std::string& device, WorkQueue& queue, ReplayStats& stats) {
    int fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (fd < 0) {
        printf("Thread %d: Failed to open device: %s\n", thread_id, strerror(errno));
        return;
    }

    // Setup libaio
    io_context_t ctx = 0;
    if (io_setup(QUEUE_DEPTH, &ctx) < 0) {
        printf("Thread %d: io_setup failed\n", thread_id);
        close(fd);
        return;
    }

    // Allocate aligned buffers
    std::vector<void*> buffers(QUEUE_DEPTH);
    std::vector<struct iocb> iocbs(QUEUE_DEPTH);
    std::vector<struct iocb*> iocb_ptrs(QUEUE_DEPTH);
    std::vector<uint32_t> io_sizes(QUEUE_DEPTH);

    // Free slot management
    std::vector<int> free_slots;
    free_slots.reserve(QUEUE_DEPTH);
    for (int i = 0; i < QUEUE_DEPTH; i++) {
        free_slots.push_back(i);
    }

    for (int i = 0; i < QUEUE_DEPTH; i++) {
        if (posix_memalign(&buffers[i], IO_BLOCK_SIZE, MAX_IO_SIZE) != 0) {
            printf("Thread %d: Failed to allocate buffer\n", thread_id);
            io_destroy(ctx);
            close(fd);
            return;
        }
        memset(buffers[i], 0, MAX_IO_SIZE);
    }

    uint64_t completed_ios = 0;
    uint64_t completed_bytes = 0;
    uint64_t batch_ios = 0;
    uint64_t batch_bytes = 0;
    int in_flight = 0;

    // Event buffer for io_getevents
    std::vector<struct io_event> events(QUEUE_DEPTH);

    WorkItem item;
    bool reading_done = false;

    while (!g_stop_flag.load()) {
        // Submit new IOs if we have free slots, work, and pending limit allows
        while (!free_slots.empty() && !reading_done) {
            // Check global pending bytes limit before popping from queue
            uint64_t current_pending = g_pending_bytes.load();
            if (current_pending >= MAX_PENDING_BYTES) {
                // Already at limit, need to reap first
                break;
            }

            if (!queue.pop(item)) {
                reading_done = true;
                break;
            }

            if (item.offset < 0) {
                reading_done = true;
                break;
            }

            // Align size
            uint32_t aligned_size = ((item.size + IO_BLOCK_SIZE - 1) / IO_BLOCK_SIZE) * IO_BLOCK_SIZE;
            if (aligned_size > MAX_IO_SIZE) aligned_size = MAX_IO_SIZE;

            // Get free slot
            int slot = free_slots.back();
            free_slots.pop_back();

            io_sizes[slot] = aligned_size;
            io_prep_pwrite(&iocbs[slot], fd, buffers[slot], aligned_size, item.offset);
            iocbs[slot].data = (void*)(intptr_t)slot;
            iocb_ptrs[0] = &iocbs[slot];

            int ret = io_submit(ctx, 1, iocb_ptrs.data());
            if (ret == 1) {
                in_flight++;
                g_pending_bytes.fetch_add(aligned_size);
            } else {
                // Submit failed, return slot to free list
                free_slots.push_back(slot);
            }
        }

        // Reap completed IOs
        if (in_flight > 0) {
            struct timespec timeout = {0, 1000000};  // 1ms
            int n = io_getevents(ctx, 1, in_flight, events.data(), &timeout);

            for (int i = 0; i < n; i++) {
                int slot = (int)(intptr_t)events[i].obj->data;
                // Return slot to free list
                free_slots.push_back(slot);
                in_flight--;
                // Decrease global pending bytes
                g_pending_bytes.fetch_sub(io_sizes[slot]);

                if (events[i].res > 0) {
                    completed_ios++;
                    completed_bytes += io_sizes[slot];
                    batch_ios++;
                    batch_bytes += io_sizes[slot];
                }
            }

            // Batch update stats every 1000 IOs
            if (batch_ios >= 1000) {
                stats.add(batch_ios, batch_bytes);
                batch_ios = 0;
                batch_bytes = 0;
                stats.report();
            }
        }

        if (reading_done && in_flight == 0) break;
    }

    // Final stats
    if (batch_ios > 0 || batch_bytes > 0) {
        stats.add(batch_ios, batch_bytes);
    }

    // Cleanup
    for (auto& buf : buffers) {
        free(buf);
    }
    io_destroy(ctx);
    close(fd);

    printf("Thread %d: Completed %lu IOs\n", thread_id, completed_ios);
}

// Reader thread
void reader_thread(const std::string& trace_path, std::vector<WorkQueue*>& queues,
                   LbaMapper& mapper, int num_threads, uint32_t io_scale) {
    std::ifstream file(trace_path);
    if (!file.is_open()) {
        printf("ERROR: Cannot open trace file: %s\n", trace_path.c_str());
        for (auto& q : queues) q->set_done();
        return;
    }

    uint64_t line_num = 0;
    uint64_t distributed = 0;
    std::string line;

    while (std::getline(file, line) && !g_stop_flag.load()) {
        // Parse CSV: timestamp, op, offset, size
        std::istringstream iss(line);
        std::string token;
        std::vector<std::string> parts;

        while (std::getline(iss, token, ',')) {
            // Trim whitespace
            size_t start = token.find_first_not_of(" \t");
            size_t end = token.find_last_not_of(" \t");
            if (start != std::string::npos) {
                parts.push_back(token.substr(start, end - start + 1));
            } else {
                parts.push_back("");
            }
        }

        if (parts.size() >= 4) {
            const std::string& op = parts[1];
            if (op == "W" || op == "WS") {
                try {
                    uint64_t offset = std::stoull(parts[2]) * io_scale;
                    uint32_t size = std::stoul(parts[3]) * io_scale;

                    if (size > 0) {
                        // Split IO into 4KB chunks and map each independently
                        uint64_t num_blocks = (size + IO_BLOCK_SIZE - 1) / IO_BLOCK_SIZE;
                        for (uint64_t i = 0; i < num_blocks; i++) {
                            uint64_t block_offset = offset + (i * IO_BLOCK_SIZE);
                            int64_t device_offset = mapper.map(block_offset);
                            if (device_offset >= 0) {
                                WorkItem item{device_offset, IO_BLOCK_SIZE};
                                int thread_id = distributed % num_threads;
                                queues[thread_id]->push(item);
                                distributed++;
                            }
                        }
                    }
                } catch (...) {
                    // Skip invalid lines
                }
            }
        }

        line_num++;
        if (line_num % 1000000 == 0) {
            if (mapper.is_remap()) {
                printf("[Reader] Parsed %lu lines, distributed %lu writes, unique %.2f GB, skipped %lu\n",
                       line_num, distributed,
                       mapper.get_unique_bytes() / (1024.0*1024*1024),
                       mapper.get_skipped_ios());
            } else {
                printf("[Reader] Parsed %lu lines, distributed %lu writes\n",
                       line_num, distributed);
            }
        }
    }

    // Signal end to all workers
    for (auto& q : queues) {
        WorkItem end_item{-1, 0};
        q->push(end_item);
        q->set_done();
    }

    if (mapper.is_remap()) {
        printf("[Reader] Done: %lu lines, %lu writes, unique %.2f GB, skipped %lu\n",
               line_num, distributed,
               mapper.get_unique_bytes() / (1024.0*1024*1024),
               mapper.get_skipped_ios());
    } else {
        printf("[Reader] Done: %lu lines, %lu writes\n",
               line_num, distributed);
    }
}

void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --device PATH    Target device (auto-detect if not specified)\n");
    printf("  --trace PATH     Trace file path (default: %s)\n", DEFAULT_TRACE);
    printf("  --threads N      Number of threads (default: %d)\n", NUM_THREADS);
    printf("  --max-tb N       Max write amount in TB (default: %.1f)\n", DEFAULT_MAX_TB);
    printf("  --remap-lba      Remap LBAs sequentially (default: passthrough)\n");
    printf("  --io-scale N     Multiply offset and size by N (default: 1)\n");
    printf("  --help           Show this help\n");
}

int main(int argc, char* argv[]) {
    // Parse arguments
    std::string device;
    std::string trace = DEFAULT_TRACE;
    int num_threads = NUM_THREADS;
    double max_tb = DEFAULT_MAX_TB;
    bool remap_lba = false;
    uint32_t io_scale = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace = argv[++i];
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-tb") == 0 && i + 1 < argc) {
            max_tb = atof(argv[++i]);
        } else if (strcmp(argv[i], "--remap-lba") == 0) {
            remap_lba = true;
        } else if (strcmp(argv[i], "--io-scale") == 0 && i + 1 < argc) {
            io_scale = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    g_max_write_bytes.store((uint64_t)(max_tb * 1024 * 1024 * 1024 * 1024));

    // Setup signal handler
    signal(SIGINT, signal_handler);

    // Auto-detect device if not specified
    if (device.empty()) {
        device = find_device();
        if (device.empty()) {
            printf("ERROR: No device found. Specify with --device\n");
            printf("\nChecked locations:\n");
            printf("  ublk:    /dev/ublkb0\n");
            printf("  NVMe-oF: ICACHE/FTLBDEV/NULL/OCF\n");
            printf("  OpenCAS: /dev/cas1-1\n");
            return 1;
        }
    }

    // Get device size
    uint64_t device_size = get_device_size(device);
    if (device_size == 0) {
        printf("ERROR: Cannot get device size\n");
        return 1;
    }

    // Check trace exists
    struct stat st;
    if (stat(trace.c_str(), &st) != 0) {
        printf("ERROR: Trace file not found: %s\n", trace.c_str());
        return 1;
    }

    printf("============================================================\n");
    printf("  High-Performance Trace Replay (C++ / libaio)\n");
    printf("============================================================\n");
    printf("  Device:      %s\n", device.c_str());
    printf("  Device size: %.2f GB (%lu LBAs)\n",
           device_size / (1024.0*1024*1024), device_size / IO_BLOCK_SIZE);
    printf("  Trace:       %s\n", trace.c_str());
    printf("  Threads:     %d\n", num_threads);
    printf("  QD/thread:   %d\n", QUEUE_DEPTH);
    printf("  Max write:   %.1f TB\n", max_tb);
    printf("  IO scale:   %u\n", io_scale);
    printf("  LBA mode:   %s\n", remap_lba ? "Remap (sequential)" : "Passthrough");
    printf("============================================================\n\n");

    // Create LBA mapper
    LbaMapper mapper(device_size, remap_lba);

    // Create work queues
    std::vector<WorkQueue*> queues;
    for (int i = 0; i < num_threads; i++) {
        queues.push_back(new WorkQueue(1024));
    }

    // Create stats
    ReplayStats stats;

    // Start worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < num_threads; i++) {
        workers.emplace_back(worker_thread, i, device, std::ref(*queues[i]), std::ref(stats));
    }

    // Start reader thread
    std::thread reader(reader_thread, trace, std::ref(queues), std::ref(mapper), num_threads, io_scale);

    printf("Replay started...\n\n");

    // Wait for reader to finish
    reader.join();

    // Wait for workers to finish
    for (auto& w : workers) {
        w.join();
    }

    stats.final_report();
    mapper.print_stats();

    // Cleanup
    for (auto& q : queues) {
        delete q;
    }

    return 0;
}
