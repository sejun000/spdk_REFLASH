#include "log_cache_wrapper.h"
#include "log_cache_config.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <strings.h>
#include <unordered_map>
#include <vector>

extern "C" {
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/bdev_zone.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme.h"
}

// Forward declaration for bdev_nvme_get_ctrlr (defined in module/bdev/nvme/bdev_nvme.h)
extern "C" struct spdk_nvme_ctrlr *bdev_nvme_get_ctrlr(struct spdk_bdev *bdev);

#include "port/cache_device.h"
#include "port/evict_policy_greedy.h"
#include "port/evict_policy_cost_benefit.h"
#include "port/evict_policy_fifo.h"
#include "port/istream.h"
#include "port/log_cache.h"
#include "port/log_cache_segment.h"
#include "logging/stats_logger.h"

// Debug flag for offset tracking - enable to trace write/read/GC/evict offsets
#define OFFSET_DEBUG 0

// Score functions for CbEvictPolicy (same as icache.cpp)
static double score_age_evict(Segment *seg) {
    return -static_cast<double>(seg->create_timestamp);
}

// Global variables for score_warm_first (set by LogCache during GC)
extern uint64_t g_threshold;
extern uint64_t g_timestamp;

static double score_warm_first(Segment *seg) {
    if (g_threshold <= 0 || g_timestamp <= 0) {
        // Fallback to simple age-based score if globals not set
        return -static_cast<double>(seg->create_timestamp);
    }
    // Use actual segment size from LogCacheSegment::blocks
    double segment_size = static_cast<double>(reinterpret_cast<LogCacheSegment*>(seg)->blocks.size());
    double u = seg->valid_cnt / segment_size;
    if (u < 0.0001) u = 0.0001;  // Avoid division by zero
    return std::min(g_threshold - (g_timestamp - seg->create_timestamp),
                    g_timestamp - seg->create_timestamp) * (1 - u) / u;
}

// Score function: prefer HOT segments (recently created) for compaction
static double score_hot_first(Segment *seg) {
    if (g_threshold <= 0 || g_timestamp <= 0) {
        return -static_cast<double>(seg->create_timestamp);
    }
    double segment_size = static_cast<double>(reinterpret_cast<LogCacheSegment*>(seg)->blocks.size());
    double u = seg->valid_cnt / segment_size;
    if (u < 0.0001) u = 0.0001;
    // Hot-first: higher score for segments with smaller age (recently created)
    return (g_threshold - (g_timestamp - seg->create_timestamp)) * (1 - u) / u;
}

// Score function: prefer COLD segments (old) for compaction
static double score_cold_first(Segment *seg) {
    if (g_threshold <= 0 || g_timestamp <= 0) {
        return -static_cast<double>(seg->create_timestamp);
    }
    double segment_size = static_cast<double>(reinterpret_cast<LogCacheSegment*>(seg)->blocks.size());
    double u = seg->valid_cnt / segment_size;
    if (u < 0.0001) u = 0.0001;
    // Cold-first: higher score for segments with larger age (older)
    return (g_timestamp - seg->create_timestamp) * (1 - u) / u;
}

// Score function for SEPBIT: sqrt of age for balanced selection
static double score_sepbit_age(Segment *seg) {
    if (g_threshold <= 0 || g_timestamp <= 0) {
        return -static_cast<double>(seg->create_timestamp);
    }
    double segment_size = static_cast<double>(reinterpret_cast<LogCacheSegment*>(seg)->blocks.size());
    double u = seg->valid_cnt / segment_size;
    if (u < 0.0001) u = 0.0001;
    return std::sqrt(static_cast<double>(g_timestamp - seg->create_timestamp)) * (1 - u) / u;
}

namespace icache {

//==============================================================================
// DMA Buffer Pool - Pre-allocated hugepage memory for GC/Evict operations
// Simple chunk-based design: 256KB chunks, LIFO stack (no sorting needed)
// - GC/Evict uses 256KB batches (64 x 4KB = 16 x 16KB aligned writes)
// - Remaining blocks < 256KB also use chunk (simple, slight waste is OK)
//==============================================================================
class DmaBufferPool {
public:
	static constexpr size_t CHUNK_SIZE = 2 * 1024 * 1024; // 2MB per chunk (for 16 x 128KB evict)
	static constexpr size_t POOL_SIZE = 2ULL * 1024 * 1024 * 1024;  // 2GB total
	static constexpr size_t NUM_CHUNKS = POOL_SIZE / CHUNK_SIZE;    // 2048 chunks

	static DmaBufferPool& instance() {
		static DmaBufferPool pool;
		return pool;
	}

	bool init() {
		if (initialized_) return true;

		// Allocate 2GB contiguous DMA-capable memory
		pool_base_ = spdk_dma_zmalloc(POOL_SIZE, 4096, nullptr);
		if (!pool_base_) {
			SPDK_ERRLOG("Failed to allocate DMA buffer pool (%zu MB)\n", POOL_SIZE / (1024 * 1024));
			return false;
		}

		// Initialize free stack with all chunks (LIFO - no sorting needed)
		free_stack_.reserve(NUM_CHUNKS);
		for (size_t i = 0; i < NUM_CHUNKS; ++i) {
			free_stack_.push_back(i);
		}
		allocated_count_ = 0;
		initialized_ = true;
		return true;
	}

	void destroy() {
		if (pool_base_) {
			spdk_dma_free(pool_base_);
			pool_base_ = nullptr;
		}
		free_stack_.clear();
		allocated_count_ = 0;
		initialized_ = false;
	}

	// Allocate one 256KB chunk (for GC batch or leftover)
	void* alloc_chunk() {
		if (!initialized_ || free_stack_.empty()) {
			SPDK_ERRLOG("DMA pool exhausted: %zu chunks in use, initialized=%d\n",
				    allocated_count_, initialized_);
			void *ptr = spdk_dma_zmalloc(CHUNK_SIZE, 4096, nullptr);
			assert(ptr != nullptr && "DMA alloc failed: pool exhausted and fallback failed");
			return ptr;
		}

		size_t chunk_idx = free_stack_.back();
		free_stack_.pop_back();
		allocated_count_++;
		return static_cast<uint8_t*>(pool_base_) + chunk_idx * CHUNK_SIZE;
	}

	// Free one 256KB chunk back to pool
	void free_chunk(void *ptr) {
		if (!ptr) return;

		uintptr_t base = reinterpret_cast<uintptr_t>(pool_base_);
		uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

		if (!initialized_ || !pool_base_ || addr < base || addr >= base + POOL_SIZE) {
			// Not from our pool
			spdk_dma_free(ptr);
			return;
		}

		size_t chunk_idx = (addr - base) / CHUNK_SIZE;
		free_stack_.push_back(chunk_idx);
		allocated_count_--;
	}

	// Legacy API for compatibility (allocates full chunks, wastes remainder)
	void* alloc(size_t size) {
		if (size > CHUNK_SIZE) {
			// Need multiple chunks - allocate externally for now
			SPDK_WARNLOG("Allocation %zu > chunk size %zu, using spdk_dma_zmalloc\n",
				     size, CHUNK_SIZE);
			return spdk_dma_zmalloc(size, 4096, nullptr);
		}
		return alloc_chunk();
	}

	void free(void *ptr, size_t size) {
		(void)size;  // Chunk-based, size ignored
		free_chunk(ptr);
	}

	size_t used() const { return allocated_count_ * CHUNK_SIZE; }
	size_t available() const { return free_stack_.size() * CHUNK_SIZE; }
	size_t free_chunks() const { return free_stack_.size(); }
	bool is_initialized() const { return initialized_; }
	void* base() const { return pool_base_; }

private:
	DmaBufferPool() = default;
	~DmaBufferPool() { destroy(); }
	DmaBufferPool(const DmaBufferPool&) = delete;
	DmaBufferPool& operator=(const DmaBufferPool&) = delete;

	void *pool_base_ = nullptr;
	std::vector<size_t> free_stack_;   // Free chunk indices (LIFO stack)
	size_t allocated_count_ = 0;       // Number of allocated chunks
	bool initialized_ = false;
};

// Global helper functions for buffer allocation
inline void* dma_pool_alloc(size_t size) {
	return DmaBufferPool::instance().alloc(size);
}

inline void dma_pool_free(void *ptr, size_t size) {
	DmaBufferPool::instance().free(ptr, size);
}

struct SyncResult {
	bool done = false;
	int status = 0;
};

struct IoWaitCtx {
	bool ready = false;
};

// Async IO context for tracking completion
struct AsyncIoCtx {
	cache_device_io_cb user_cb;
	void *user_cb_arg;
	uint64_t offset;      // For error logging
	size_t len;           // For error logging
	bool is_read;         // true = read, false = write
	bool is_cache;        // true = cache device, false = backend device
};

// Forward declaration
class SpdkCacheDevice;

// Global device command queue depth limiting
// Disabled: mqes=1023 is large enough, no throttling needed
static constexpr uint32_t MAX_OUTSTANDING_CMDS = UINT64_MAX;
static std::atomic<uint32_t> g_outstanding_cmds{0};

struct GlobalPendingWrite {
	SpdkCacheDevice *device;
	uint64_t zone_id;
	uint64_t offset;
	struct iovec *iovs;
	int iovcnt;
	size_t total_len;
	cache_device_io_cb cb;
	void *cb_arg;
	int placement_handle;
};
static std::deque<GlobalPendingWrite> g_global_pending_writes;

struct GlobalPendingRead {
	SpdkCacheDevice *device;
	uint64_t offset;
	void *buf;
	size_t len;
	cache_device_io_cb cb;
	void *cb_arg;
	bool is_iov;  // true = readv, false = read
	struct iovec *iovs;
	int iovcnt;
};
static std::deque<GlobalPendingRead> g_global_pending_reads;

static std::atomic<bool> g_draining_pending_writes{false};  // Prevent recursive drain for writes
static std::atomic<bool> g_draining_pending_reads{false};   // Prevent recursive drain for reads

// Forward declaration for drain functions
static void drain_global_pending_writes();
static void drain_global_pending_reads();

// Zone reset async context
struct ZoneResetAsyncCtx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_bdev *bdev;
	uint64_t start_zone;      // First zone being reset (for ZoneQueue update)
	uint64_t current_zone;
	uint64_t end_zone;
	uint64_t zone_size_blocks;
	cache_device_io_cb user_cb;
	void *user_cb_arg;
	int last_status;
	SpdkCacheDevice *device;  // For updating ZoneQueue state
	uint64_t start_tsc;       // For measuring reset time
	int zones_reset;          // Count of zones reset
};

#if FDP && FDP_TRIM
// FDP trim async context
struct FdpTrimAsyncCtx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	uint64_t block_offset;
	uint64_t num_blocks;
	uint64_t zone_size_blocks;
	cache_device_io_cb user_cb;
	void *user_cb_arg;
	SpdkCacheDevice *device;
	uint64_t start_tsc;
};
#endif

// Zone Queue Entry - pending IO for a zone
struct ZoneQueueEntry {
	uint64_t offset;
	const void *buf;
	size_t len;
	cache_device_io_cb cb;
	void *cb_arg;
	bool is_write;
	struct iovec *iovs;  // For writev pending queue
	int iovcnt;          // For writev pending queue
	int placement_handle;  // FDP placement handle
};

// Zone Queue - manages WP-based throttling per zone
static constexpr size_t ZONE_MAX_LBA_DISTANCE = 1 * 1024 * 1024;  // 1MB
static constexpr size_t ZRWAFG_ALIGN = 16 * 1024;  // 16KB alignment for ZRWAFG=4

struct InflightIo {
	uint64_t end_offset;
	bool completed;
};

struct ZoneQueue {
	uint64_t write_pointer = 0;                   // WP: end of hole-free completed region
	uint64_t flushed_wp = 0;                      // WP that has been flushed to device
	bool flush_in_progress = false;              // Flush command in flight
	bool zone_opened = false;                    // Zone opened with ZRWA
	bool open_in_progress = false;               // Zone open command in flight
	bool is_fdp = false;                         // FDP mode: skip ZRWA constraints
	std::map<uint64_t, InflightIo> inflight_ios;  // offset -> {end_offset, completed}
	std::queue<ZoneQueueEntry> pending;           // Pending IOs waiting for this zone

	bool is_aligned(uint64_t offset, size_t len) const {
		return (offset % ZRWAFG_ALIGN == 0) && (len % ZRWAFG_ALIGN == 0);
	}

	// Check if zone is ready for writes (opened with ZRWA)
	bool is_zone_ready() const {
		return zone_opened && !open_in_progress;
	}

	// Check if we need to open the zone first
	bool needs_zone_open() const {
		return !zone_opened && !open_in_progress;
	}

	bool can_submit(uint64_t offset, size_t len, bool debug = false) const {
		// Can't submit if zone is not opened
		if (!zone_opened) {
			if (debug) {
				SPDK_NOTICELOG("can_submit: FAIL zone_opened=false, offset=%lu\n", offset);
			}
			return false;
		}
		// FDP mode: no ZRWA constraints, can write anywhere in zone
		if (is_fdp) {
			return true;
		}
		// Can't submit if flush is in progress (wait for device WP to advance)
		if (flush_in_progress) {
			if (debug) {
				SPDK_NOTICELOG("can_submit: FAIL flush_in_progress, offset=%lu\n", offset);
			}
			return false;
		}
		// Can't write to already written area (ZNS sequential write constraint)
		if (offset < write_pointer) {
			if (debug) {
				SPDK_NOTICELOG("can_submit: FAIL offset=%lu < WP=%lu\n", offset, write_pointer);
			}
			return false;
		}
		if (is_aligned(offset, len)) {
			// Aligned: can submit if within ZRWA window from flushed_wp (device WP)
			bool ok = offset + len <= flushed_wp + ZONE_MAX_LBA_DISTANCE;
			if (debug && !ok) {
				SPDK_NOTICELOG("can_submit: FAIL aligned offset+len=%lu > flushed_wp+1MB=%lu\n",
					       offset + len, flushed_wp + ZONE_MAX_LBA_DISTANCE);
			}
			return ok;
		} else {
			// Non-aligned: can only submit if it's exactly at write_pointer
			bool ok = offset == write_pointer;
			if (debug && !ok) {
				SPDK_NOTICELOG("can_submit: FAIL non-aligned offset=%lu != WP=%lu\n", offset, write_pointer);
			}
			return ok;
		}
	}

	void add_inflight(uint64_t offset, size_t len) {
		inflight_ios[offset] = {offset + len, false};
	}

	// Remove inflight IO on error (no WP update)
	void remove_inflight(uint64_t offset) {
		inflight_ios.erase(offset);
	}

	// Complete IO successfully and update WP
	// Returns true if WP advanced and flush is needed
	bool complete_io(uint64_t offset) {
		auto it = inflight_ios.find(offset);
		if (it != inflight_ios.end()) {
			it->second.completed = true;
		}
		return update_write_pointer();
	}

	// Returns true if WP advanced (flush may be needed)
	bool update_write_pointer() {
		uint64_t old_wp = write_pointer;
		// Advance WP past contiguous completed IOs starting from current WP
		while (!inflight_ios.empty()) {
			auto it = inflight_ios.begin();
			// Check if the first inflight IO starts at WP and is completed
			if (it->first == write_pointer && it->second.completed) {
				write_pointer = it->second.end_offset;
				inflight_ios.erase(it);
			} else {
				break;
			}
		}
		return write_pointer > old_wp;
	}

	// Check if flush is needed (WP advanced beyond flushed WP)
	bool needs_flush() const {
		return !flush_in_progress && write_pointer > flushed_wp;
	}

	// Get the WP to flush to (in bytes)
	uint64_t get_flush_target_wp() const {
		return write_pointer;
	}
};

static void
sync_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	auto *res = static_cast<SyncResult *>(cb_arg);
	res->status = success ? 0 : -EIO;
	res->done = true;
	spdk_bdev_free_io(bdev_io);
}

static void
async_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	auto *ctx = static_cast<AsyncIoCtx *>(cb_arg);
	int status = success ? 0 : -EIO;
	spdk_bdev_free_io(bdev_io);
	if (!success) {
		SPDK_ERRLOG("async_io_completion: IO FAILED! %s %s offset=0x%lx len=%zu\n",
			    ctx->is_read ? "READ" : "WRITE",
			    ctx->is_cache ? "CACHE" : "BACKEND",
			    ctx->offset, ctx->len);
	}
	// Decrement outstanding cmd count for cache reads
	if (ctx->is_cache && ctx->is_read) {
		g_outstanding_cmds.fetch_sub(1);
		// Drain pending reads and writes
		drain_global_pending_reads();
		drain_global_pending_writes();
	}
	// Cache write completion: also trigger drain (NVMe queue slot freed)
	if (ctx->is_cache && !ctx->is_read) {
		drain_global_pending_reads();
		drain_global_pending_writes();
	}
	if (ctx->user_cb) {
		ctx->user_cb(ctx->user_cb_arg, status);
	}
	delete ctx;
}

static void
io_wait_cb(void *arg)
{
	auto *ctx = static_cast<IoWaitCtx *>(arg);
	ctx->ready = true;
}

// Forward declaration for zone reset chain
static void zone_reset_next(ZoneResetAsyncCtx *ctx);

// Forward declaration for clearing zone state after reset
static void clear_zone_state_after_reset(SpdkCacheDevice *device, uint64_t start_zone,
					 uint64_t end_zone, uint64_t zone_size_blocks);

static void
zone_reset_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	auto *ctx = static_cast<ZoneResetAsyncCtx *>(cb_arg);
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		SPDK_ERRLOG("zone_reset_completion: Zone reset failed!\n");
		assert(false && "Zone reset failed");
		ctx->last_status = -EIO;
	}

	// Move to next zone
	ctx->current_zone += ctx->zone_size_blocks;
	ctx->zones_reset++;

	if (ctx->current_zone <= ctx->end_zone) {
		// More zones to reset
		zone_reset_next(ctx);
	} else {
		// // All zones reset - log elapsed time
		// uint64_t elapsed_tsc = spdk_get_ticks() - ctx->start_tsc;
		// uint64_t elapsed_us = elapsed_tsc * 1000000 / spdk_get_ticks_hz();
		// SPDK_NOTICELOG("Zone reset complete: %d zones, %lu us (%.2f ms/zone)\n",
		// 	       ctx->zones_reset, elapsed_us,
		// 	       (double)elapsed_us / 1000.0 / ctx->zones_reset);

		// Clear ZoneQueue state
		if (ctx->device && ctx->last_status == 0) {
			clear_zone_state_after_reset(ctx->device, ctx->start_zone,
						     ctx->end_zone, ctx->zone_size_blocks);
		}
		// All done
		if (ctx->user_cb) {
			ctx->user_cb(ctx->user_cb_arg, ctx->last_status);
		}
		delete ctx;
	}
}

static void
zone_reset_next(ZoneResetAsyncCtx *ctx)
{
	uint64_t zone_idx = ctx->current_zone / ctx->zone_size_blocks;
	uint64_t end_zone_idx = ctx->end_zone / ctx->zone_size_blocks;
	SPDK_NOTICELOG("zone_reset_next: zone_idx=%lu (block=%lu), end_zone_idx=%lu\n",
		       zone_idx, ctx->current_zone, end_zone_idx);
	int rc = spdk_bdev_zone_management(ctx->desc, ctx->ch, ctx->current_zone,
					   SPDK_BDEV_ZONE_RESET, zone_reset_completion, ctx);
	if (rc) {
		// Failed to submit, complete with error
		SPDK_ERRLOG("zone_reset_next: spdk_bdev_zone_management failed with rc=%d\n", rc);
		if (ctx->user_cb) {
			ctx->user_cb(ctx->user_cb_arg, rc);
		}
		delete ctx;
	}
}

#if FDP && FDP_TRIM
// Forward declaration for clearing zone state after FDP trim
static void clear_zone_state_after_fdp_trim(SpdkCacheDevice *device, uint64_t block_offset,
					    uint64_t num_blocks, uint64_t zone_size_blocks);

static void
fdp_trim_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	auto *ctx = static_cast<FdpTrimAsyncCtx *>(cb_arg);
	spdk_bdev_free_io(bdev_io);

	int status = 0;
	if (!success) {
		SPDK_ERRLOG("fdp_trim_completion: TRIM/UNMAP failed!\n");
		status = -EIO;
	} else {
		// uint64_t elapsed_tsc = spdk_get_ticks() - ctx->start_tsc;
		// uint64_t elapsed_us = elapsed_tsc * 1000000 / spdk_get_ticks_hz();
		// SPDK_NOTICELOG("FDP TRIM complete: block_offset=0x%lx, num_blocks=%lu, %lu us\n",
		// 	       ctx->block_offset, ctx->num_blocks, elapsed_us);

		// Clear zone state for reuse
		if (ctx->device) {
			clear_zone_state_after_fdp_trim(ctx->device, ctx->block_offset,
						       ctx->num_blocks, ctx->zone_size_blocks);
		}
	}

	if (ctx->user_cb) {
		ctx->user_cb(ctx->user_cb_arg, status);
	}
	delete ctx;
}
#endif

class SpdkCacheDevice : public CacheDeviceInterface {
public:
	SpdkCacheDevice(struct spdk_bdev_desc *cache_desc,
			struct spdk_bdev_desc *backend_desc,
			uint32_t block_size)
		: m_cache_desc(cache_desc),
		  m_backend_desc(backend_desc),
		  m_block_size(block_size)
	{
		m_cache_bdev = spdk_bdev_desc_get_bdev(m_cache_desc);
		m_backend_bdev = spdk_bdev_desc_get_bdev(m_backend_desc);
		m_cache_max_rw = 0;
		m_backend_max_rw = 0;

		// Detect ZNS from bdev, fallback to hardcoded if not detected
		m_cache_zoned = spdk_bdev_is_zoned(m_cache_bdev);
		if (m_cache_zoned) {
			m_cache_zone_blocks = spdk_bdev_get_zone_size(m_cache_bdev);
		} else {
#if FDP
			// FDP mode: keep m_cache_zoned = false
			// This skips zone open, ZRWA flush, and zone reset commands
			m_cache_zone_blocks = 0x80000;  // 524288 blocks = 2GB (virtual zone size for eviction)
			SPDK_NOTICELOG("FDP mode enabled: ZNS commands disabled\n");
#else
			// Fallback: force ZNS mode with hardcoded zone size
			m_cache_zoned = true;
			m_cache_zone_blocks = 0x80000;  // 524288 blocks = 2GB zone size
#endif
		}

	}

	void set_channels(struct spdk_io_channel *cache_ch, struct spdk_io_channel *backend_ch)
	{
		m_cache_ch = cache_ch;
		m_backend_ch = backend_ch;
	}

	int write_cache(uint64_t offset, const void *buf, size_t len) override
	{
		return submit_rw(m_cache_desc, m_cache_ch, buf, len, offset, true, m_cache_max_rw);
	}

	int read_cache(uint64_t offset, void *buf, size_t len) override
	{
		return submit_rw(m_cache_desc, m_cache_ch, buf, len, offset, false, m_cache_max_rw);
	}

	int reset_cache_region(uint64_t offset, size_t len) override
	{
		// Disabled: use reset_cache_region_async instead to avoid reentrant spdk_thread_poll
		(void)offset;
		(void)len;
		return 0;
	}

	int write_backend(uint64_t offset, const void *buf, size_t len) override
	{
		return submit_rw(m_backend_desc, m_backend_ch, buf, len, offset, true, m_backend_max_rw);
	}

	int read_backend(uint64_t offset, void *buf, size_t len) override
	{
		return submit_rw(m_backend_desc, m_backend_ch, buf, len, offset, false, m_backend_max_rw);
	}

	int trim_backend(uint64_t offset, size_t len) override
	{
		// Disabled: synchronous trim causes reentrant spdk_thread_poll crash
		return 0;
	}

private:
	// Zone queue management
	std::unordered_map<uint64_t, ZoneQueue> zone_queues_;  // zone_id -> ZoneQueue

	// Get zone ID from byte offset
	uint64_t get_zone_id(uint64_t byte_offset) const {
		if (m_cache_zone_blocks == 0) {
			return 0;  // Not ZNS, single zone
		}
		uint64_t zone_size_bytes = m_cache_zone_blocks * m_block_size;
		return byte_offset / zone_size_bytes;
	}

	// Zone-aware async completion callback context
	struct ZoneAsyncIoCtx {
		static constexpr uint32_t MAGIC = 0xDEADBEEF;
		static constexpr uint32_t FREED_MAGIC = 0xFEEDFACE;
		uint32_t magic = MAGIC;
		SpdkCacheDevice *device;
		uint64_t zone_id;
		uint64_t io_offset;  // Offset of this IO for inflight tracking
		cache_device_io_cb user_cb;
		void *user_cb_arg;
	};

	static int convert_to_blocks(struct spdk_bdev *bdev, uint64_t byte_offset, size_t len,
				     uint64_t *offset_blocks, uint64_t *num_blocks)
	{
		const uint32_t block_size = spdk_bdev_get_block_size(bdev);
		if (block_size == 0) {
			return -EINVAL;
		}
		if ((byte_offset % block_size) || (len % block_size)) {
			return -EINVAL;
		}
		*offset_blocks = byte_offset / block_size;
		*num_blocks = len / block_size;
		return 0;
	}

	int submit_rw(struct spdk_bdev_desc *desc,
		      struct spdk_io_channel *ch,
		      const void *buf,
		      size_t len,
		      uint64_t byte_offset,
		      bool write,
		      uint32_t max_rw)
	{
		if (len == 0) {
			return 0;
		}
		if (!ch) {
			return -EINVAL;
		}
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(bdev, byte_offset, len, &block_offset, &num_blocks)) {
			return -EINVAL;
		}
		if (num_blocks == 0) {
			return 0;
		}

		uint64_t remaining = num_blocks;
		uint64_t current_block = block_offset;
		const uint8_t *buf_ro = static_cast<const uint8_t *>(buf);
		uint8_t *buf_rw = const_cast<uint8_t *>(buf_ro);

		while (remaining > 0) {
			uint64_t chunk_blocks = max_rw ? std::min<uint64_t>(remaining, max_rw) : remaining;
			size_t chunk_bytes = chunk_blocks * m_block_size;
			SyncResult res;
			int rc;
			if (write) {
				rc = spdk_bdev_write_blocks(desc, ch, const_cast<uint8_t *>(buf_ro),
							    current_block, chunk_blocks,
							    sync_io_completion, &res);
			} else {
				rc = spdk_bdev_read_blocks(desc, ch, buf_rw,
							   current_block, chunk_blocks,
							   sync_io_completion, &res);
			}
			if (rc == -ENOMEM) {
				IoWaitCtx wait_ctx{};
				struct spdk_bdev_io_wait_entry wait_entry = {};
				wait_entry.bdev = bdev;
				wait_entry.cb_arg = &wait_ctx;
				wait_entry.cb_fn = io_wait_cb;
				rc = spdk_bdev_queue_io_wait(bdev, ch, &wait_entry);
				if (rc) {
					return rc;
				}
				while (!wait_ctx.ready) {
					spdk_thread_poll(spdk_get_thread(), 0, 0);
				}
				continue;
			} else if (rc) {
				return rc;
			}

			while (!res.done) {
				spdk_thread_poll(spdk_get_thread(), 0, 0);
			}
			if (res.status) {
				return res.status;
			}

			if (write) {
				buf_ro += chunk_bytes;
			} else {
				buf_rw += chunk_bytes;
			}
			current_block += chunk_blocks;
			remaining -= chunk_blocks;
		}
		return 0;
	}

	struct spdk_bdev_desc *m_cache_desc;
	struct spdk_bdev_desc *m_backend_desc;
	struct spdk_bdev *m_cache_bdev = nullptr;
	struct spdk_bdev *m_backend_bdev = nullptr;
	struct spdk_io_channel *m_cache_ch = nullptr;
	struct spdk_io_channel *m_backend_ch = nullptr;
	uint32_t m_cache_max_rw = 0;
	uint32_t m_backend_max_rw = 0;
	uint64_t m_cache_zone_blocks = 0;
	bool m_cache_zoned = false;
	uint32_t m_block_size;

public:
	// Zone capacity in blocks (writable blocks per zone)
	// Hardcoded for the test device: 0x43500 = 275712 blocks
	static constexpr uint64_t ZONE_CAPACITY_BLOCKS = 0x43500;

	// Get zone capacity in bytes (actual writable space per zone)
	// Always return hardcoded value for ZNS device
	uint64_t zone_capacity_bytes() const {
		if (m_cache_zone_blocks > 0) {
			// ZNS device - use hardcoded capacity
			return ZONE_CAPACITY_BLOCKS * m_block_size;
		}
		return 0;
	}

	// Get zone size in bytes (for physical_base alignment)
	uint64_t zone_size_bytes() const {
		if (m_cache_zone_blocks > 0) {
			return m_cache_zone_blocks * m_block_size;
		}
		return 0;
	}

	// Get cache bdev for NVMe controller access
	struct spdk_bdev *get_cache_bdev() const { return m_cache_bdev; }

	// Async API implementations
	// Zone-aware write: QD1 per zone
	int write_cache_async(uint64_t offset, const void *buf, size_t len,
			      cache_device_io_cb cb, void *cb_arg) override
	{

		if (len == 0) {
			if (cb) cb(cb_arg, 0);
			return 0;
		}

		// Get zone ID for this write
		uint64_t zone_id = get_zone_id(offset);
		ZoneQueue &zq = zone_queues_[zone_id];

		// Check if zone needs to be opened with ZRWA first
		if (zq.needs_zone_open()) {
			// Queue this write first
			ZoneQueueEntry entry;
			entry.offset = offset;
			entry.buf = buf;
			entry.len = len;
			entry.cb = cb;
			entry.cb_arg = cb_arg;
			entry.is_write = true;
			entry.iovs = nullptr;
			entry.iovcnt = 0;
			zq.pending.push(entry);

			// Trigger zone open with ZRWA
			zq.open_in_progress = true;
			uint64_t zone_slba = zone_id * m_cache_zone_blocks;
			open_zone_zrwa_async(zone_slba, nullptr, nullptr);
			return 0;
		}

		// Check if we can submit based on WP and alignment
		// Aligned (16KB): can submit if within 1MB from WP
		// Non-aligned: can only submit if offset == WP
		// pending이 있으면 순서 보장을 위해 queue에 넣음
		if (!zq.pending.empty() || !zq.can_submit(offset, len)) {
			ZoneQueueEntry entry;
			entry.offset = offset;
			entry.buf = buf;
			entry.len = len;
			entry.cb = cb;
			entry.cb_arg = cb_arg;
			entry.is_write = true;
			entry.iovs = nullptr;  // Not a writev request
			entry.iovcnt = 0;
			zq.pending.push(entry);
			return 0;  // Queued successfully
		}

		// Can submit, add to inflight and send
		zq.add_inflight(offset, len);
		return submit_zone_io_direct(zone_id, offset, buf, len, true, cb, cb_arg);
	}

	// Scatter-gather write to cache (16KB aligned, no memcpy)
	// Uses pending queue if can't submit immediately
	// placement_handle: FDP placement handle (0 ~ FDP_NUM_PLACEMENT_HANDLES-1)
	int writev_cache_async(uint64_t offset, struct iovec *iovs, int iovcnt, size_t total_len,
			       cache_device_io_cb cb, void *cb_arg, int placement_handle = 0)
	{

		if (!m_cache_ch || total_len == 0) {
			if (cb) cb(cb_arg, 0);
			return 0;
		}

		uint64_t zone_id = get_zone_id(offset);
		ZoneQueue &zq = zone_queues_[zone_id];

		// Check if zone needs to be opened with ZRWA first
		if (zq.needs_zone_open()) {
			// Queue this write first
			ZoneQueueEntry entry;
			entry.offset = offset;
			entry.buf = nullptr;
			entry.len = total_len;
			entry.cb = cb;
			entry.cb_arg = cb_arg;
			entry.is_write = true;
			entry.iovs = iovs;
			entry.iovcnt = iovcnt;
			entry.placement_handle = placement_handle;
			zq.pending.push(entry);

			// Trigger zone open with ZRWA
			zq.open_in_progress = true;
			uint64_t zone_slba = zone_id * m_cache_zone_blocks;
			open_zone_zrwa_async(zone_slba, nullptr, nullptr);
			return 0;
		}

		// pending이 있으면 순서 보장을 위해 queue에 넣음 (ZRWA에서도 WP 관리를 위해)
		if (!zq.pending.empty() || !zq.can_submit(offset, total_len)) {
			// Queue it for later - store iovec info
			ZoneQueueEntry entry;
			entry.offset = offset;
			entry.buf = nullptr;  // Not used for writev
			entry.len = total_len;
			entry.cb = cb;
			entry.cb_arg = cb_arg;
			entry.is_write = true;
			entry.iovs = iovs;
			entry.iovcnt = iovcnt;
			entry.placement_handle = placement_handle;
			zq.pending.push(entry);
			return 0;
		}

		return submit_writev_direct(zone_id, offset, iovs, iovcnt, total_len, cb, cb_arg, placement_handle);
	}

	int submit_writev_direct(uint64_t zone_id, uint64_t offset, struct iovec *iovs, int iovcnt,
				 size_t total_len, cache_device_io_cb cb, void *cb_arg, int placement_handle = 0)
	{
		assert(iovs != nullptr && "submit_writev_direct: iovs is NULL");
		for (int i = 0; i < iovcnt; ++i) {
			assert(iovs[i].iov_base != nullptr && "submit_writev_direct: iov_base is NULL");
		}

		// Check global command queue depth limit
		// Also queue if pending is not empty to maintain FIFO order
		// Skip pending check if called from drain context
		uint32_t current_cmds = g_outstanding_cmds.load();
		bool has_pending = !g_draining_pending_writes.load() && !g_global_pending_writes.empty();
		if (current_cmds >= MAX_OUTSTANDING_CMDS || has_pending) {
			// Queue to global pending - will be drained on completion
			GlobalPendingWrite pending;
			pending.device = this;
			pending.zone_id = zone_id;
			pending.offset = offset;
			pending.iovs = iovs;
			pending.iovcnt = iovcnt;
			pending.total_len = total_len;
			pending.cb = cb;
			pending.cb_arg = cb_arg;
			pending.placement_handle = placement_handle;
			g_global_pending_writes.push_back(pending);

			// Try to drain immediately if we have capacity
			drain_global_pending_writes();
			return 0;  // Queued successfully, callback will be called later
		}

		// Increment outstanding command count
		g_outstanding_cmds.fetch_add(1);

		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(m_cache_bdev, offset, total_len, &block_offset, &num_blocks)) {
			g_outstanding_cmds.fetch_sub(1);
			SPDK_ERRLOG("submit_writev_direct: convert_to_blocks failed offset=%lu len=%zu\n",
				    offset, total_len);
			return -EINVAL;
		}

		ZoneQueue &zq = zone_queues_[zone_id];
		zq.add_inflight(offset, total_len);

		auto *ctx = new (std::nothrow) ZoneAsyncIoCtx();
		if (!ctx) {
			zq.remove_inflight(offset);
			g_outstanding_cmds.fetch_sub(1);
			SPDK_ERRLOG("submit_writev_direct: failed to allocate ZoneAsyncIoCtx\n");
			return -ENOMEM;
		}
		ctx->device = this;
		ctx->zone_id = zone_id;
		ctx->io_offset = offset;
		ctx->user_cb = cb;
		ctx->user_cb_arg = cb_arg;

		int rc;
#if FDP
		// FDP mode: use extended write with placement handle
		struct spdk_bdev_ext_io_opts opts = {};
		opts.size = sizeof(opts);
		opts.nvme_cdw12.write.dtype = 2;  // Directive Type = FDP (Data Placement)
#if FDP_PLACEMENT_ENABLED
		opts.nvme_cdw13.write.dspec = static_cast<uint16_t>(placement_handle);  // Placement Handle ID
#else
		opts.nvme_cdw13.write.dspec = 0;  // Force all writes to placement_handle=0
#endif

		// SPDK_NOTICELOG("FDP submit: placement_handle=%d, cdw12.raw=0x%x, cdw13.raw=0x%x, opts.size=%zu\n",
		// 	       placement_handle, opts.nvme_cdw12.raw, opts.nvme_cdw13.raw, opts.size);

		rc = spdk_bdev_writev_blocks_ext(m_cache_desc, m_cache_ch, iovs, iovcnt,
						 block_offset, num_blocks,
						 zone_async_io_completion, ctx, &opts);
#else
		rc = spdk_bdev_writev_blocks(m_cache_desc, m_cache_ch, iovs, iovcnt,
					     block_offset, num_blocks,
					     zone_async_io_completion, ctx);
#endif
		if (rc) {
			delete ctx;
			zq.remove_inflight(offset);
			g_outstanding_cmds.fetch_sub(1);
			SPDK_ERRLOG("submit_writev_direct: bdev write failed rc=%d offset=%lu\n", rc, offset);
			return rc;
		}
		return 0;
	}

	int read_cache_async(uint64_t offset, void *buf, size_t len,
			     cache_device_io_cb cb, void *cb_arg) override
	{
		return submit_rw_async(m_cache_desc, m_cache_ch, m_cache_bdev,
				       buf, len, offset, false, cb, cb_arg);
	}

	int readv_cache_async(uint64_t offset, struct iovec *iovs, int iovcnt,
			      size_t total_len, cache_device_io_cb cb, void *cb_arg) override
	{
		if (total_len == 0 || iovcnt == 0) {
			if (cb) cb(cb_arg, 0);
			return 0;
		}
		if (!m_cache_ch) {
			return -EINVAL;
		}

		// Check global outstanding cmd limit
		uint32_t current_cmds = g_outstanding_cmds.load();
		bool has_pending = !g_draining_pending_reads.load() && !g_global_pending_reads.empty();
		if (current_cmds >= MAX_OUTSTANDING_CMDS || has_pending) {
			// Queue to pending reads
			GlobalPendingRead pending;
			pending.device = this;
			pending.offset = offset;
			pending.buf = nullptr;
			pending.len = total_len;
			pending.cb = cb;
			pending.cb_arg = cb_arg;
			pending.is_iov = true;
			pending.iovs = iovs;
			pending.iovcnt = iovcnt;
			g_global_pending_reads.push_back(pending);

			drain_global_pending_reads();
			return 0;  // Queued successfully
		}

		return submit_readv_direct(offset, iovs, iovcnt, total_len, cb, cb_arg);
	}

	int submit_readv_direct(uint64_t offset, struct iovec *iovs, int iovcnt,
				size_t total_len, cache_device_io_cb cb, void *cb_arg)
	{
		g_outstanding_cmds.fetch_add(1);

		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(m_cache_bdev, offset, total_len, &block_offset, &num_blocks)) {
			g_outstanding_cmds.fetch_sub(1);
			return -EINVAL;
		}
		if (num_blocks == 0) {
			g_outstanding_cmds.fetch_sub(1);
			if (cb) cb(cb_arg, 0);
			return 0;
		}

		auto *async_ctx = new (std::nothrow) AsyncIoCtx();
		if (!async_ctx) {
			g_outstanding_cmds.fetch_sub(1);
			return -ENOMEM;
		}
		async_ctx->user_cb = cb;
		async_ctx->user_cb_arg = cb_arg;
		async_ctx->offset = offset;
		async_ctx->len = total_len;
		async_ctx->is_read = true;
		async_ctx->is_cache = true;

		int rc = spdk_bdev_readv_blocks(m_cache_desc, m_cache_ch,
						iovs, iovcnt,
						block_offset, num_blocks,
						async_io_completion, async_ctx);
		if (rc) {
			g_outstanding_cmds.fetch_sub(1);
			delete async_ctx;
			return rc;
		}
		return 0;
	}

	// Public wrapper for drain_global_pending_reads to submit cache read directly
	int submit_read_cache_direct(uint64_t offset, void *buf, size_t len,
				     cache_device_io_cb cb, void *cb_arg)
	{
		return submit_rw_async_direct(m_cache_desc, m_cache_ch, m_cache_bdev,
					      buf, len, offset, false, cb, cb_arg);
	}

	int write_backend_async(uint64_t offset, const void *buf, size_t len,
				cache_device_io_cb cb, void *cb_arg) override
	{
		return submit_rw_async(m_backend_desc, m_backend_ch, m_backend_bdev,
				       buf, len, offset, true, cb, cb_arg);
	}

	int writev_backend_async(uint64_t offset, struct iovec *iovs, int iovcnt,
				 size_t total_len, cache_device_io_cb cb, void *cb_arg) override
	{
		if (total_len == 0 || iovcnt == 0) {
			if (cb) cb(cb_arg, 0);
			return 0;
		}
		if (!m_backend_ch) {
			return -EINVAL;
		}

		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(m_backend_bdev, offset, total_len, &block_offset, &num_blocks)) {
			return -EINVAL;
		}
		if (num_blocks == 0) {
			if (cb) cb(cb_arg, 0);
			return 0;
		}

		auto *async_ctx = new (std::nothrow) AsyncIoCtx();
		if (!async_ctx) {
			return -ENOMEM;
		}
		async_ctx->user_cb = cb;
		async_ctx->user_cb_arg = cb_arg;

		int rc = spdk_bdev_writev_blocks(m_backend_desc, m_backend_ch,
						 iovs, iovcnt,
						 block_offset, num_blocks,
						 async_io_completion, async_ctx);
		if (rc) {
			delete async_ctx;
			return rc;
		}
		return 0;
	}

	int read_backend_async(uint64_t offset, void *buf, size_t len,
			       cache_device_io_cb cb, void *cb_arg) override
	{
		return submit_rw_async(m_backend_desc, m_backend_ch, m_backend_bdev,
				       buf, len, offset, false, cb, cb_arg);
	}

	// Deferred callback context for FDP mode reset
	struct DeferredResetCtx {
		cache_device_io_cb cb;
		void *cb_arg;
	};

	static void deferred_reset_cb(void *arg) {
		auto *ctx = static_cast<DeferredResetCtx*>(arg);
		if (ctx->cb) {
			ctx->cb(ctx->cb_arg, 0);
		}
		delete ctx;
	}

	int reset_cache_region_async(uint64_t offset, size_t len,
				     cache_device_io_cb cb, void *cb_arg) override
	{
		// Check if ZNS and valid parameters
		// For FDP mode (m_cache_zoned=false): send TRIM if FDP_TRIM enabled, else just clear zone state
		if (!m_cache_zoned || len == 0 || m_cache_zone_blocks == 0) {
#if FDP && FDP_TRIM
			// FDP mode with TRIM enabled: send TRIM/UNMAP command
			if (len > 0 && m_cache_ch) {
				uint64_t block_offset, num_blocks;
				if (convert_to_blocks(m_cache_bdev, offset, len, &block_offset, &num_blocks) == 0 &&
				    num_blocks > 0) {
					auto *ctx = new (std::nothrow) FdpTrimAsyncCtx();
					if (!ctx) {
						if (cb) cb(cb_arg, -ENOMEM);
						return 0;
					}
					ctx->desc = m_cache_desc;
					ctx->ch = m_cache_ch;
					ctx->block_offset = block_offset;
					ctx->num_blocks = num_blocks;
					ctx->zone_size_blocks = m_cache_zone_blocks;
					ctx->user_cb = cb;
					ctx->user_cb_arg = cb_arg;
					ctx->device = this;
					// ctx->start_tsc = spdk_get_ticks();

					// SPDK_NOTICELOG("FDP TRIM: block_offset=0x%lx, num_blocks=%lu\n",
					// 	       block_offset, num_blocks);
					int rc = spdk_bdev_unmap_blocks(m_cache_desc, m_cache_ch,
								       block_offset, num_blocks,
								       fdp_trim_completion, ctx);
					if (rc) {
						SPDK_ERRLOG("FDP TRIM failed to submit: rc=%d\n", rc);
						delete ctx;
						if (cb) cb(cb_arg, rc);
					}
					return 0;
				}
			}
#else
			// FDP mode without TRIM: just clear zone state for reuse
			if (m_cache_zone_blocks > 0 && len > 0) {
				uint64_t start_zone = offset / (m_cache_zone_blocks * m_block_size);
				uint64_t end_zone = (offset + len - 1) / (m_cache_zone_blocks * m_block_size);
				for (uint64_t z = start_zone; z <= end_zone; ++z) {
					clear_zone_state(z);
				}
			}
#endif
			// Defer callback to avoid nested callback chain
			if (cb) {
				auto *ctx = new DeferredResetCtx{cb, cb_arg};
				spdk_thread_send_msg(spdk_get_thread(), deferred_reset_cb, ctx);
			}
			return 0;
		}

		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(m_cache_bdev, offset, len, &block_offset, &num_blocks)) {
			if (cb) {
				cb(cb_arg, -EINVAL);
			}
			return 0;
		}

		if (num_blocks == 0 || !m_cache_ch) {
			if (cb) {
				cb(cb_arg, 0);
			}
			return 0;
		}

		uint64_t start_zone = spdk_bdev_get_zone_id(m_cache_bdev, block_offset);
		uint64_t end_zone = spdk_bdev_get_zone_id(m_cache_bdev, block_offset + num_blocks - 1);
		SPDK_NOTICELOG("reset_cache_region_async: offset=0x%lx, len=%zu, block_offset=0x%lx, num_blocks=%lu\n",
		       offset, len, block_offset, num_blocks);
		SPDK_NOTICELOG("  start_zone=%lu, end_zone=%lu, zone_size_blocks=%lu\n",
		       start_zone, end_zone, m_cache_zone_blocks);

		// Create async context
		auto *ctx = new (std::nothrow) ZoneResetAsyncCtx();
		if (!ctx) {
			if (cb) {
				cb(cb_arg, -ENOMEM);
			}
			return 0;
		}

		ctx->desc = m_cache_desc;
		ctx->ch = m_cache_ch;
		ctx->bdev = m_cache_bdev;
		ctx->start_zone = start_zone;
		ctx->current_zone = start_zone;
		ctx->end_zone = end_zone;
		ctx->zone_size_blocks = m_cache_zone_blocks;
		ctx->user_cb = cb;
		ctx->user_cb_arg = cb_arg;
		ctx->last_status = 0;
		ctx->device = this;
		// ctx->start_tsc = spdk_get_ticks();
		ctx->zones_reset = 0;

		// Start first zone reset
		zone_reset_next(ctx);
		return 0;
	}

private:
	int submit_rw_async(struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch,
			    struct spdk_bdev *bdev,
			    const void *buf,
			    size_t len,
			    uint64_t byte_offset,
			    bool write,
			    cache_device_io_cb cb,
			    void *cb_arg)
	{
		if (len == 0) {
			if (cb) cb(cb_arg, 0);
			return 0;
		}
		assert(buf != nullptr && "submit_rw_async: buffer is NULL");
		if (!ch) {
			return -EINVAL;
		}

		bool is_cache = (desc == m_cache_desc);
		bool is_read = !write;

		// Check global outstanding cmd limit for cache reads
		if (is_cache && is_read) {
			uint32_t current_cmds = g_outstanding_cmds.load();
			bool has_pending = !g_draining_pending_reads.load() && !g_global_pending_reads.empty();
			if (current_cmds >= MAX_OUTSTANDING_CMDS || has_pending) {
				// Queue to pending reads
				GlobalPendingRead pending;
				pending.device = this;
				pending.offset = byte_offset;
				pending.buf = const_cast<void*>(buf);
				pending.len = len;
				pending.cb = cb;
				pending.cb_arg = cb_arg;
				pending.is_iov = false;
				pending.iovs = nullptr;
				pending.iovcnt = 0;
				g_global_pending_reads.push_back(pending);

				drain_global_pending_reads();
				return 0;  // Queued successfully
			}
		}

		return submit_rw_async_direct(desc, ch, bdev, buf, len, byte_offset, write, cb, cb_arg);
	}

	int submit_rw_async_direct(struct spdk_bdev_desc *desc,
				   struct spdk_io_channel *ch,
				   struct spdk_bdev *bdev,
				   const void *buf,
				   size_t len,
				   uint64_t byte_offset,
				   bool write,
				   cache_device_io_cb cb,
				   void *cb_arg)
	{
		bool is_cache = (desc == m_cache_desc);
		bool is_read = !write;

		if (is_cache && is_read) {
			g_outstanding_cmds.fetch_add(1);
		}

		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(bdev, byte_offset, len, &block_offset, &num_blocks)) {
			if (is_cache && is_read) g_outstanding_cmds.fetch_sub(1);
			return -EINVAL;
		}
		if (num_blocks == 0) {
			if (is_cache && is_read) g_outstanding_cmds.fetch_sub(1);
			if (cb) cb(cb_arg, 0);
			return 0;
		}

		auto *async_ctx = new (std::nothrow) AsyncIoCtx();
		if (!async_ctx) {
			if (is_cache && is_read) g_outstanding_cmds.fetch_sub(1);
			return -ENOMEM;
		}
		async_ctx->user_cb = cb;
		async_ctx->user_cb_arg = cb_arg;
		async_ctx->offset = byte_offset;
		async_ctx->len = len;
		async_ctx->is_read = is_read;
		async_ctx->is_cache = is_cache;

		int rc;
		if (write) {
			rc = spdk_bdev_write_blocks(desc, ch,
						    const_cast<void *>(buf),
						    block_offset, num_blocks,
						    async_io_completion, async_ctx);
		} else {
			rc = spdk_bdev_read_blocks(desc, ch,
						   const_cast<void *>(buf),
						   block_offset, num_blocks,
						   async_io_completion, async_ctx);
		}

		if (rc) {
			if (is_cache && is_read) g_outstanding_cmds.fetch_sub(1);
			delete async_ctx;
			return rc;
		}
		return 0;
	}

	// Submit IO directly to device (internal, inflight already added by caller)
	int submit_zone_io_direct(uint64_t zone_id, uint64_t offset, const void *buf, size_t len,
				  bool write, cache_device_io_cb cb, void *cb_arg)
	{
		assert(buf != nullptr && "submit_zone_io_direct: buffer is NULL");
		if (!m_cache_ch) {
			zone_queues_[zone_id].remove_inflight(offset);
			return -EINVAL;
		}

		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(m_cache_bdev, offset, len, &block_offset, &num_blocks)) {
			zone_queues_[zone_id].remove_inflight(offset);
			return -EINVAL;
		}

		if (num_blocks == 0) {
			zone_queues_[zone_id].complete_io(offset);
			if (cb) cb(cb_arg, 0);
			process_zone_pending(zone_id);
			return 0;
		}

		// Increment outstanding command count (will be decremented in zone_async_io_completion)
		g_outstanding_cmds.fetch_add(1);

		// Allocate context for zone-aware completion
		auto *zone_ctx = new (std::nothrow) ZoneAsyncIoCtx();
		if (!zone_ctx) {
			g_outstanding_cmds.fetch_sub(1);
			zone_queues_[zone_id].remove_inflight(offset);
			return -ENOMEM;
		}
		zone_ctx->device = this;
		zone_ctx->zone_id = zone_id;
		zone_ctx->io_offset = offset;
		zone_ctx->user_cb = cb;
		zone_ctx->user_cb_arg = cb_arg;

		int rc;
		if (write) {
			rc = spdk_bdev_write_blocks(m_cache_desc, m_cache_ch,
						    const_cast<void *>(buf),
						    block_offset, num_blocks,
						    zone_async_io_completion, zone_ctx);
		} else {
			rc = spdk_bdev_read_blocks(m_cache_desc, m_cache_ch,
						   const_cast<void *>(buf),
						   block_offset, num_blocks,
						   zone_async_io_completion, zone_ctx);
		}

		if (rc) {
			g_outstanding_cmds.fetch_sub(1);
			delete zone_ctx;
			zone_queues_[zone_id].remove_inflight(offset);
			return rc;
		}
		return 0;
	}

	// Static callback for zone-aware async IO completion
	static void zone_async_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
	{

		auto *ctx = static_cast<ZoneAsyncIoCtx *>(cb_arg);

		// Verify magic to detect double-free or corruption
		if (ctx->magic == ZoneAsyncIoCtx::FREED_MAGIC) {
			SPDK_ERRLOG("zone_async_io_completion: double callback detected! ctx=%p\n", ctx);
			assert(false && "Double callback detected");
			spdk_bdev_free_io(bdev_io);
			return;  // Already freed, do not process again
		}
		if (ctx->magic != ZoneAsyncIoCtx::MAGIC) {
			SPDK_ERRLOG("zone_async_io_completion: invalid magic 0x%x, ctx=%p\n", ctx->magic, ctx);
			assert(false && "Invalid magic in callback context");
			spdk_bdev_free_io(bdev_io);
			return;  // Corrupted context
		}

		SpdkCacheDevice *device = ctx->device;
		uint64_t zone_id = ctx->zone_id;
		uint64_t io_offset = ctx->io_offset;
		cache_device_io_cb user_cb = ctx->user_cb;
		void *user_cb_arg = ctx->user_cb_arg;


		spdk_bdev_free_io(bdev_io);

		// Mark as freed before delete to detect double-free
		ctx->magic = ZoneAsyncIoCtx::FREED_MAGIC;
		delete ctx;

		// Decrement global outstanding command count
		uint32_t prev_cmds = g_outstanding_cmds.fetch_sub(1);
		if (prev_cmds == 0) {
			SPDK_ERRLOG("zone_async_io_completion: g_outstanding_cmds underflow!\n");
		}

		// Drain pending reads and writes (NVMe queue slot freed)

		int status = success ? 0 : -EIO;
		if (!success) {
			SPDK_ERRLOG("zone_async_io_completion: IO failed! zone_id=%lu, io_offset=%lu\n", zone_id, io_offset);
			assert(false && "Zone async IO failed");
		}

		// Call user callback (may trigger new writes to this zone)
		if (user_cb) {
			user_cb(user_cb_arg, status);
		}

		// Complete IO (updates WP) and process pending queue
		uint64_t old_wp = device->zone_queues_[zone_id].write_pointer;
		bool wp_advanced = device->zone_queues_[zone_id].complete_io(io_offset);
		uint64_t new_wp = device->zone_queues_[zone_id].write_pointer;
		if (wp_advanced) {
		}
		device->process_zone_pending(zone_id);

		// If WP advanced, flush ZRWA to commit writes to device
		if (wp_advanced) {
			device->maybe_flush_zrwa(zone_id);
		}

		// Drain global pending writes if any
		drain_global_pending_reads();
		drain_global_pending_writes();
	}

	// Process pending IOs for a zone based on WP and alignment
	void process_zone_pending(uint64_t zone_id)
	{
		ZoneQueue &zq = zone_queues_[zone_id];

		while (!zq.pending.empty()) {
			const ZoneQueueEntry &entry = zq.pending.front();

			// Check if we can submit based on WP and alignment
			if (!zq.can_submit(entry.offset, entry.len)) {
				break;
			}


			// Can submit, pop and send
			ZoneQueueEntry submit_entry = zq.pending.front();
			zq.pending.pop();

			int rc;
			if (submit_entry.iovs != nullptr && submit_entry.iovcnt > 0) {
				// This is a writev request - use submit_writev_direct
				rc = submit_writev_direct(zone_id, submit_entry.offset,
							  submit_entry.iovs, submit_entry.iovcnt,
							  submit_entry.len, submit_entry.cb, submit_entry.cb_arg,
							  submit_entry.placement_handle);
			} else {
				// Regular write request
				zq.add_inflight(submit_entry.offset, submit_entry.len);
				rc = submit_zone_io_direct(zone_id, submit_entry.offset, submit_entry.buf,
							   submit_entry.len, submit_entry.is_write,
							   submit_entry.cb, submit_entry.cb_arg);
			}
			if (rc != 0) {
				// Submit failed, call user callback with error
				if (submit_entry.cb) {
					submit_entry.cb(submit_entry.cb_arg, rc);
				}
				// Continue to try next one
			}
		}
	}

	//==========================================================================
	// ZRWA Flush Explicit - commits writes to advance device WP
	//==========================================================================
	struct ZrwaFlushCtx {
		SpdkCacheDevice *device;
		uint64_t zone_id;
		uint64_t flush_wp;  // WP we're flushing to
	};

	static void zrwa_flush_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
	{
		auto *ctx = static_cast<ZrwaFlushCtx *>(cb_arg);
		SpdkCacheDevice *device = ctx->device;
		uint64_t zone_id = ctx->zone_id;
		uint64_t flush_wp = ctx->flush_wp;

		spdk_bdev_free_io(bdev_io);

		ZoneQueue &zq = device->zone_queues_[zone_id];
		zq.flush_in_progress = false;

		if (success) {
			// Update flushed WP (device WP now advanced)
			zq.flushed_wp = flush_wp;
		} else {
			SPDK_ERRLOG("ZRWA flush failed: zone=%lu\n", zone_id);
			assert(false && "ZRWA flush failed");
		}

		delete ctx;

		// Process pending writes first (they were waiting for flushed_wp to advance)
		device->process_zone_pending(zone_id);

		// Then check if more flush is needed (WP may have advanced from pending writes)
		if (zq.needs_flush()) {
			device->flush_zrwa_async(zone_id);
		}
	}

public:
	// Flush ZRWA for a zone - commits all writes up to current WP
	// ZRWA Flush Explicit: CDW10-11 = end LBA (not zone SLBA!)
	// Flush range: [current device WP, end LBA], then device WP = end LBA + 1
	int flush_zrwa_async(uint64_t zone_id)
	{
		ZoneQueue &zq = zone_queues_[zone_id];

		if (!zq.needs_flush()) {
			return 0;  // Nothing to flush
		}

		if (!m_cache_zoned) {
			// Not a ZNS device, no flush needed
			zq.flushed_wp = zq.write_pointer;
			return 0;
		}

		zq.flush_in_progress = true;
		uint64_t flush_wp = zq.get_flush_target_wp();  // byte offset

		// Calculate end LBA for ZRWA Flush with ZRWAFG alignment
		// Flush range must be a multiple of ZRWAFG (4 blocks = 16KB)
		static constexpr uint64_t ZRWAFG_BLOCKS = 4;

		uint64_t device_wp_blocks = zq.flushed_wp / m_block_size;
		uint64_t flush_end_blocks = flush_wp / m_block_size;  // exclusive end
		uint64_t flush_count = flush_end_blocks - device_wp_blocks;

		// Align flush count down to ZRWAFG boundary
		uint64_t aligned_count = (flush_count / ZRWAFG_BLOCKS) * ZRWAFG_BLOCKS;
		if (aligned_count == 0) {
			// Not enough data to flush (less than ZRWAFG)
			zq.flush_in_progress = false;
			return 0;
		}

		uint64_t end_lba = device_wp_blocks + aligned_count - 1;
		// Update flush_wp to match aligned end (for flushed_wp update on completion)
		flush_wp = (end_lba + 1) * m_block_size;


		// Build NVMe Zone Management Send command (Flush Explicit = 0x11)
		struct spdk_nvme_cmd cmd = {};
		cmd.opc = SPDK_NVME_OPC_ZONE_MGMT_SEND;  // 0x79
		// CDW10-11: end LBA of flush range (not zone SLBA!)
		*(uint64_t *)&cmd.cdw10 = end_lba;
		cmd.cdw13 = 0x11;  // Flush Explicit (ZRWA commit)

		auto *ctx = new (std::nothrow) ZrwaFlushCtx{this, zone_id, flush_wp};
		if (!ctx) {
			zq.flush_in_progress = false;
			return -ENOMEM;
		}

		int rc = spdk_bdev_nvme_io_passthru(m_cache_desc, m_cache_ch,
						    &cmd, nullptr, 0,
						    zrwa_flush_completion, ctx);
		if (rc != 0) {
			delete ctx;
			zq.flush_in_progress = false;
			SPDK_ERRLOG("Failed to submit ZRWA flush: rc=%d\n", rc);
		}

		return rc;
	}

	// Check and flush ZRWA if needed for a zone
	// Triggers async flush if SW WP > device WP (flushed_wp)
	void maybe_flush_zrwa(uint64_t zone_id)
	{
		ZoneQueue &zq = zone_queues_[zone_id];
		if (zq.needs_flush()) {
			flush_zrwa_async(zone_id);
		}
	}

	//==========================================================================
	// Zone Open with ZRWA - opens zone with ZRWA allocation
	//==========================================================================
	struct ZoneOpenCtx {
		SpdkCacheDevice *device;
		uint64_t zone_id;
		cache_device_io_cb user_cb;
		void *user_cb_arg;
	};

	static void zone_open_zrwa_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
	{

		auto *ctx = static_cast<ZoneOpenCtx *>(cb_arg);
		SpdkCacheDevice *device = ctx->device;
		cache_device_io_cb user_cb = ctx->user_cb;
		void *user_cb_arg = ctx->user_cb_arg;
		uint64_t zone_id = ctx->zone_id;


		spdk_bdev_free_io(bdev_io);

		// Update zone state
		ZoneQueue &zq = device->zone_queues_[zone_id];
		zq.open_in_progress = false;

		int status = success ? 0 : -EIO;
		if (success) {
			zq.zone_opened = true;
			// Initialize WP to zone start offset (byte unit)
			uint64_t zone_start_offset = zone_id * device->m_cache_zone_blocks * device->m_block_size;
			zq.write_pointer = zone_start_offset;
			zq.flushed_wp = zone_start_offset;
		} else {
			SPDK_ERRLOG("Failed to open zone %lu with ZRWA\n", zone_id);
			assert(false && "Zone open with ZRWA failed");
		}

		delete ctx;

		// Call user callback first
		if (user_cb) {
			user_cb(user_cb_arg, status);
		}

		// Process pending writes now that zone is open
		if (success) {
			device->process_zone_pending(zone_id);
		}
	}

public:
	// Open a zone with ZRWA (Zone Random Write Area) allocation
	// zone_slba: Starting LBA of the zone (in blocks)
	int open_zone_zrwa_async(uint64_t zone_slba, cache_device_io_cb cb, void *cb_arg)
	{

		if (!m_cache_zoned) {
			// FDP mode: no zone open needed, no ZRWA constraints
			uint64_t zone_id = zone_slba / (m_cache_zone_blocks > 0 ? m_cache_zone_blocks : 1);
			ZoneQueue &zq = zone_queues_[zone_id];
			zq.zone_opened = true;
			zq.is_fdp = true;  // Skip ZRWA constraints in can_submit()
			zq.open_in_progress = false;
			// Initialize WP (not used in FDP mode but keep for consistency)
			uint64_t zone_start_offset = zone_id * m_cache_zone_blocks * m_block_size;
			zq.write_pointer = zone_start_offset;
			zq.flushed_wp = zone_start_offset;
			if (cb) {
				cb(cb_arg, 0);
			}
			process_zone_pending(zone_id);
			return 0;
		}

		uint64_t zone_id = zone_slba / m_cache_zone_blocks;

		auto *ctx = new (std::nothrow) ZoneOpenCtx{this, zone_id, cb, cb_arg};
		if (!ctx) {
			if (cb) {
				cb(cb_arg, -ENOMEM);
			}
			return -ENOMEM;
		}

		// Try NVMe passthru with ZRWA first
		struct spdk_nvme_cmd cmd = {};
		cmd.opc = SPDK_NVME_OPC_ZONE_MGMT_SEND;  // 0x79
		*(uint64_t *)&cmd.cdw10 = zone_slba;
		cmd.cdw13 = 0x3 | (1 << 9);  // OPEN + ZRWAA

		int rc = spdk_bdev_nvme_io_passthru(m_cache_desc, m_cache_ch,
						    &cmd, nullptr, 0,
						    zone_open_zrwa_completion, ctx);

		if (rc == -ENOTSUP || rc == -EOPNOTSUPP) {
			// Passthru not supported, fallback to zone_management (no ZRWA)
			rc = spdk_bdev_zone_management(m_cache_desc, m_cache_ch,
						       zone_slba, SPDK_BDEV_ZONE_OPEN,
						       zone_open_zrwa_completion, ctx);
		}

		if (rc != 0) {
			delete ctx;
			SPDK_ERRLOG("Failed to submit Zone Open: rc=%d\n", rc);
			if (cb) {
				cb(cb_arg, rc);
			}
		}

		return rc;
	}

	// Open zone by byte offset (convenience wrapper)
	int open_zone_zrwa_by_offset_async(uint64_t byte_offset, cache_device_io_cb cb, void *cb_arg)
	{
		if (!m_cache_zoned || m_cache_zone_blocks == 0) {
			if (cb) {
				cb(cb_arg, 0);
			}
			return 0;
		}

		// Convert byte offset to zone SLBA
		uint64_t block_offset = byte_offset / m_block_size;
		uint64_t zone_slba = (block_offset / m_cache_zone_blocks) * m_cache_zone_blocks;

		return open_zone_zrwa_async(zone_slba, cb, cb_arg);
	}

	// Clear zone state after zone reset (called from zone_reset_completion)
	void clear_zone_state(uint64_t zone_id)
	{
		auto it = zone_queues_.find(zone_id);
		if (it != zone_queues_.end()) {
			ZoneQueue &zq = it->second;
			zq.zone_opened = false;
			zq.is_fdp = false;  // Will be set again when zone is re-opened
			zq.open_in_progress = false;
			zq.write_pointer = 0;
			zq.flushed_wp = 0;
			zq.flush_in_progress = false;
			// Clear inflight IOs (should be empty if reset is done properly)
			zq.inflight_ios.clear();
			// Note: pending queue is NOT cleared - pending writes will trigger zone open again
		}
	}

	uint64_t get_zone_size_blocks() const { return m_cache_zone_blocks; }
};

// Implementation of clear_zone_state_after_reset (called from zone_reset_completion)
static void clear_zone_state_after_reset(SpdkCacheDevice *device, uint64_t start_zone,
					 uint64_t end_zone, uint64_t zone_size_blocks)
{
	if (!device) return;
	for (uint64_t zone_slba = start_zone; zone_slba <= end_zone; zone_slba += zone_size_blocks) {
		uint64_t zone_id = zone_slba / zone_size_blocks;
		device->clear_zone_state(zone_id);
	}
}

#if FDP && FDP_TRIM
// Implementation of clear_zone_state_after_fdp_trim (called from fdp_trim_completion)
static void clear_zone_state_after_fdp_trim(SpdkCacheDevice *device, uint64_t block_offset,
					    uint64_t num_blocks, uint64_t zone_size_blocks)
{
	if (!device || zone_size_blocks == 0) return;
	uint64_t start_zone_id = block_offset / zone_size_blocks;
	uint64_t end_zone_id = (block_offset + num_blocks - 1) / zone_size_blocks;
	for (uint64_t zone_id = start_zone_id; zone_id <= end_zone_id; ++zone_id) {
		device->clear_zone_state(zone_id);
	}
}
#endif

// Drain global pending writes when command slots become available
static void drain_global_pending_writes()
{
	// Skip if throttling disabled (UINT64_MAX means no limit)
	if (MAX_OUTSTANDING_CMDS == UINT64_MAX) {
		return;
	}

	// Prevent recursive drain (completion callbacks may trigger more drains)
	bool expected = false;
	if (!g_draining_pending_writes.compare_exchange_strong(expected, true)) {
		return;  // Already draining
	}

	size_t drained = 0;
	while (!g_global_pending_writes.empty()) {
		// Check if we have room to submit
		uint32_t current_cmds = g_outstanding_cmds.load();
		if (current_cmds >= MAX_OUTSTANDING_CMDS) {
			break;  // Still at limit, stop draining
		}

		// Pop front and submit
		GlobalPendingWrite pending = g_global_pending_writes.front();
		g_global_pending_writes.pop_front();

		// Try to submit - this will increment g_outstanding_cmds on success
		// Note: submit_writev_direct may re-queue if we hit the limit again (race)
		int rc = pending.device->submit_writev_direct(
			pending.zone_id, pending.offset, pending.iovs, pending.iovcnt,
			pending.total_len, pending.cb, pending.cb_arg, pending.placement_handle);

		if (rc != 0) {
			SPDK_ERRLOG("drain_global_pending_writes: submit failed rc=%d offset=%lu\n",
				    rc, pending.offset);
			// Call the callback with error
			if (pending.cb) {
				pending.cb(pending.cb_arg, rc);
			}
			// Free iovs if allocated
			if (pending.iovs) {
				free(pending.iovs);
			}
		}
		drained++;
	}


	g_draining_pending_writes.store(false);
}

// Drain global pending reads when command slots become available
static void drain_global_pending_reads()
{
	// Skip if throttling disabled (UINT64_MAX means no limit)
	if (MAX_OUTSTANDING_CMDS == UINT64_MAX) {
		return;
	}

	// Prevent recursive drain (completion callbacks may trigger more drains)
	bool expected = false;
	if (!g_draining_pending_reads.compare_exchange_strong(expected, true)) {
		return;  // Already draining
	}

	while (!g_global_pending_reads.empty()) {
		// Check if we have room to submit
		uint32_t current_cmds = g_outstanding_cmds.load();
		if (current_cmds >= MAX_OUTSTANDING_CMDS) {
			break;  // Still at limit, stop draining
		}

		// Pop front and submit
		GlobalPendingRead pending = g_global_pending_reads.front();
		g_global_pending_reads.pop_front();

		int rc;
		if (pending.is_iov) {
			rc = pending.device->submit_readv_direct(
				pending.offset, pending.iovs, pending.iovcnt,
				pending.len, pending.cb, pending.cb_arg);
		} else {
			rc = pending.device->submit_read_cache_direct(
				pending.offset, pending.buf, pending.len,
				pending.cb, pending.cb_arg);
		}

		if (rc != 0) {
			SPDK_ERRLOG("drain_global_pending_reads: submit failed rc=%d offset=%lu\n",
				    rc, pending.offset);
			// Call the callback with error
			if (pending.cb) {
				pending.cb(pending.cb_arg, rc);
			}
		}
	}

	g_draining_pending_reads.store(false);
}

} // namespace icache

class LogCacheAsync;  // Forward declaration

struct log_cache_ctx {
	std::unique_ptr<icache::SpdkCacheDevice> device;
	std::unique_ptr<LogCacheAsync> cache;
	uint32_t block_size = 0;
	struct spdk_io_channel *cache_ch = nullptr;
	struct spdk_io_channel *backend_ch = nullptr;
	struct spdk_poller *write_buffer_poller = nullptr;
};

//==============================================================================
// IO State Enums
//==============================================================================
enum class CacheIoState {
	HOST_WRITE_SUBMIT,
	HOST_WRITE_CACHE_WRITE,      // Writing to cache device
	HOST_WRITE_WAIT_GC_EVICT,    // Waiting for GC/Evict to complete
	HOST_WRITE_DONE,
	HOST_READ_SUBMIT,
	HOST_READ_CACHE_READ,        // Reading from cache device
	HOST_READ_BACKEND_READ,      // Reading from backend device
	HOST_READ_DONE,
};

enum class GcIoState {
	GC_SEGMENT_SUBMIT,
	READ_GC_SUBMIT,              // Reading valid blocks from cache
	WRITE_GC_SUBMIT,             // Writing to new segment
	WRITE_GC_DONE,
	GC_SEGMENT_DONE,
};

enum class EvictIoState {
	EVICT_SEGMENT_SUBMIT,
	BACKEND_READ_BLOCK,          // Reading blocks (cache valid + backend invalid)
	BACKEND_WRITE_BLOCK,         // Writing 64k chunk to backend
	BACKEND_WRITE_DONE,
	EVICT_SEGMENT_DONE,
};

//==============================================================================
// Forward declarations for callbacks
//==============================================================================
struct CacheIo;
struct GcIo;
struct EvictIo;

static void cache_io_state_machine(CacheIo *io);
static void gc_io_state_machine(GcIo *io);
static void evict_io_state_machine(EvictIo *io);

//==============================================================================
// CacheIo - Host Read/Write
//==============================================================================
struct CacheIo {
	CacheIoState state;
	log_cache_ctx *ctx;
	log_cache_io_done_cb cb_fn;
	void *cb_arg;

	// IO parameters
	uint64_t lba;
	const struct iovec *iovs;
	int iovcnt;
	size_t total_len;
	bool is_write;

	// For multi-block IO tracking
	size_t current_block_idx;
	size_t total_blocks;
	size_t completed_blocks;
	int last_status;

	// For parallel read
	std::atomic<size_t> parallel_reads_done{0};

	// For write buffer: track flushed blocks across multiple flushes
	std::atomic<size_t> flushed_blocks{0};

	// Payloads for batch operations
	std::vector<LogCache::BlockPayload> payloads;
};

//==============================================================================
// GcIo - Garbage Collection (one segment)
// Uses LogCache::GcPrepareResult internally
//==============================================================================
struct GcIo {
	GcIoState state;
	log_cache_ctx *ctx;

	// Prepare result from LogCache
	LogCache::GcPrepareResult prepare_result;

	// Progress tracking
	size_t completed_reads;
	size_t completed_writes;
	int last_status;

	// Timing for QoS throttle
	uint64_t start_ticks;
	uint64_t total_gc_bytes;
	uint64_t read_ticks;    // Total read time
	uint64_t write_ticks;   // Total write time
	uint64_t read_start;    // Batch read start
	uint64_t write_start;   // Batch write start

	// Batch processing (1MB = 256 blocks at a time for cache device)
	static constexpr size_t BATCH_BLOCKS = 256;  // 1MB / 4KB
	size_t batch_start;      // Current batch start index in blocks_to_copy
	size_t batch_count;      // Number of blocks in current batch

	// Sequential read optimization (when valid_ratio >= 0.5)
	double valid_ratio;
	bool use_sequential_read;
	uint64_t segment_base_offset;
	size_t segment_size_blocks;

	// 64KB chunks and leftover tracking (for ZNS write rules)
	static constexpr size_t BLOCKS_PER_64K = 16;  // 64KB / 4KB
	size_t num_64k_chunks;       // Number of 64KB aligned chunks in current batch
	size_t leftover_blocks;      // Number of leftover blocks (0-15)
	size_t completed_64k_writes; // Completed 64KB chunk writes
	size_t expected_64k_writes;  // Total expected writes (including stripe-split individual writes)
	size_t current_leftover_idx; // Current leftover index being written (sequential)

	// Coalesced read tracking (back merge) - for scattered read mode
	struct CoalescedRead {
		uint64_t src_offset;   // Start offset
		size_t num_blocks;     // Number of consecutive blocks
		size_t first_idx;      // First block index in batch
	};
	std::vector<CoalescedRead> coalesced_reads;
	size_t completed_coalesced_reads;

	// Sequential read mode (128k chunks) - for high valid_ratio
	static constexpr size_t SEQ_CHUNK_BLOCKS = 32;  // 128KB / 4KB
	static constexpr size_t SEQ_PARALLEL_CHUNKS = 8;  // 8 * 128KB = 1MB parallel reads
	static constexpr size_t GC_WRITES_PER_YIELD = 8;  // Yield after 8 writes (32KB)
	struct SeqChunk {
		size_t chunk_idx;      // 128k chunk index in segment
		std::vector<size_t> valid_block_indices;  // indices in blocks_to_copy
	};
	std::vector<SeqChunk> seq_chunks;  // 128k chunks with valid blocks
	size_t seq_current_chunk;          // Current chunk being processed
	size_t seq_parallel_start;         // Start of current parallel batch
	size_t seq_parallel_count;         // Number of chunks in current batch
	std::atomic<size_t> seq_reads_done;
	std::atomic<size_t> seq_writes_done;
	size_t seq_total_writes;           // Total writes expected in current batch

	// Pending write queue for yield-based dispatch
	struct PendingGcWrite {
		uint64_t dst_offset;
		void *src;
		uint32_t block_size;
	};
	std::vector<PendingGcWrite> pending_gc_writes;
	size_t pending_gc_write_idx;       // Current index in pending_gc_writes

	// Staging buffer for current batch (DMA-capable, hugepage)
	void *staging;
	size_t staging_size;

	// Completion callback for pending writes
	std::function<void(int)> on_complete;

	GcIo() : batch_start(0), batch_count(0), valid_ratio(0), use_sequential_read(false),
	         segment_base_offset(0), segment_size_blocks(0),
	         num_64k_chunks(0), leftover_blocks(0),
	         completed_64k_writes(0), expected_64k_writes(0), current_leftover_idx(0),
	         completed_coalesced_reads(0),
	         seq_current_chunk(0), seq_parallel_start(0), seq_parallel_count(0),
	         seq_reads_done(0), seq_writes_done(0), seq_total_writes(0),
	         pending_gc_write_idx(0),
	         staging(nullptr), staging_size(0),
	         start_ticks(0), total_gc_bytes(0), read_ticks(0), write_ticks(0),
	         read_start(0), write_start(0) {}
	~GcIo() {
		if (staging) {
			icache::dma_pool_free(staging, staging_size);
		}
	}
};

//==============================================================================
// EvictIo - Eviction (one segment, flush to backend)
// Uses LogCache::EvictPrepareResult internally
//==============================================================================
struct EvictIo {
	EvictIoState state;
	log_cache_ctx *ctx;

	// Prepare result from LogCache
	LogCache::EvictPrepareResult prepare_result;

	// Progress tracking
	size_t current_chunk_idx;
	size_t completed_reads;
	size_t completed_writes;
	int last_status;

	// Parallel chunk processing - process N 128k chunks at once
	static constexpr size_t PARALLEL_CHUNKS = 16;   // 16 * 128k = 2MB
	static constexpr size_t CHUNK_SIZE = 32 * 4096; // 128k per chunk
	static constexpr size_t EVICT_WRITES_PER_YIELD = 8;  // Yield after 8 writes (32KB)
	size_t parallel_batch_start;   // Start index of current parallel batch
	size_t parallel_batch_count;   // Number of chunks in current batch
	std::atomic<size_t> parallel_reads_done;   // Atomic counter for parallel reads
	std::atomic<size_t> parallel_writes_done;  // Atomic counter for parallel writes

	// LBA coalescing - track coalesced writes separately
	size_t coalesced_writes_total;  // Total coalesced writes issued

	// Pending write queue for yield-based dispatch
	struct PendingEvictWrite {
		uint64_t backend_offset;
		void *src;
		uint32_t block_size;
	};
	std::vector<PendingEvictWrite> pending_evict_writes;

	// Merged write for adjacent blocks
	struct MergedEvictWrite {
		uint64_t backend_offset;      // Starting offset
		std::vector<struct iovec> iovs;  // Scatter-gather list
		size_t total_len;             // Total bytes
	};
	std::vector<MergedEvictWrite> merged_evict_writes;
	size_t merged_evict_write_idx;  // Current index in merged_evict_writes

	// Timing for segment evict
	uint64_t start_ticks;
	uint64_t read_total_us;   // Accumulated read time
	uint64_t write_total_us;  // Accumulated write time
	uint64_t batch_read_start_ticks;  // Per-batch read start
	uint64_t batch_write_start_ticks; // Per-batch write start (after all reads done)
	size_t total_bytes;

	// Segment info for read strategy
	uint64_t segment_base_offset;  // Physical base of victim segment
	size_t segment_size_blocks;    // Total blocks in segment
	double valid_ratio;            // valid_cnt / segment_size
	bool use_sequential_read;      // true if valid_ratio >= 50%

	// Batch processing (256KB = 64 blocks at a time)
	// Matches DmaBufferPool::CHUNK_SIZE for efficient allocation
	static constexpr size_t BATCH_BLOCKS = 64;  // 256KB / 4KB
	size_t batch_start;      // Current batch start within chunk
	size_t batch_count;      // Number of blocks in current batch

	// Staging buffer (DMA-capable, hugepage) - now sized for PARALLEL_CHUNKS
	void *staging;
	size_t staging_size;

	// Completion callback
	std::function<void(int)> on_complete;

	EvictIo() : batch_start(0), batch_count(0), parallel_batch_start(0),
	            parallel_batch_count(0), parallel_reads_done(0), parallel_writes_done(0),
	            coalesced_writes_total(0), merged_evict_write_idx(0),
	            start_ticks(0), read_total_us(0), write_total_us(0),
	            batch_read_start_ticks(0), batch_write_start_ticks(0), total_bytes(0), segment_base_offset(0),
	            segment_size_blocks(0), valid_ratio(0), use_sequential_read(false),
	            staging(nullptr), staging_size(0) {}
	~EvictIo() {
		if (staging) {
			icache::dma_pool_free(staging, staging_size);
		}
	}
};

//==============================================================================
// LogCacheAsync - Async wrapper around LogCache
//==============================================================================
class LogCacheAsync {
public:
	LogCacheAsync(uint64_t cold_capacity,
		      uint64_t cache_block_count,
		      int block_size,
		      uint64_t zone_size_bytes,
		      uint64_t zone_capacity_bytes,
		      const std::string& waf_log_path,
		      const std::string& stat_log_path,
		      double valid_rate_threshold,
		      const std::string& cache_type,
		      icache::SpdkCacheDevice *device)
		: block_size_(block_size),
		  device_(device),
		  gc_in_progress_(false),
		  evict_in_progress_(false),
		  zone_size_bytes_(zone_size_bytes),
		  cache_type_(cache_type)
	{
		std::string waf_path = waf_log_path;

		// Open WAF log file
		if (!waf_path.empty()) {
			waf_fp_ = fopen(waf_path.c_str(), "w");
			if (waf_fp_) {
				fprintf(waf_fp_, "# host_write_bytes gc_write_bytes waf\n");
				fflush(waf_fp_);
			}
		}

		// Create stats logger with policy name
		stats_logger_ = std::make_unique<StatsLogger>(cache_type_, "logging");

		// Zone configuration for ZNS vs FDP
		static constexpr uint64_t ZNS_ZONE_SIZE_BLOCKS = 0x80000;      // 524288 blocks = 2GB
		static constexpr uint64_t ZNS_ZONE_CAPACITY_BLOCKS = 0x43500;  // 275712 blocks = ~1.07GB
		static constexpr uint64_t FDP_ZONE_SIZE_BLOCKS = 0x180000;     // 1572864 blocks = 6GB

#if FDP
		// FDP mode: zone_size == zone_capacity (no holes in address space)
		zone_size_bytes = FDP_ZONE_SIZE_BLOCKS * block_size;
		zone_capacity_bytes = FDP_ZONE_SIZE_BLOCKS * block_size;  // Same as zone_size!
		SPDK_NOTICELOG("FDP mode: zone_size=zone_capacity=%lu bytes\n", zone_size_bytes);
#else
		// ZNS mode: zone_capacity < zone_size
		zone_size_bytes = ZNS_ZONE_SIZE_BLOCKS * block_size;
		zone_capacity_bytes = ZNS_ZONE_CAPACITY_BLOCKS * block_size;
		SPDK_NOTICELOG("ZNS mode: zone_size=%lu, zone_capacity=%lu bytes\n",
			       zone_size_bytes, zone_capacity_bytes);
#endif

		// ZNS with striping: segment spans STRIPE_WIDTH zones
		// segment_bytes = zone_capacity * stripe_width (for legacy, but now calculated in LogCache)
		// zone_capacity_bytes = writable capacity per zone
		// zone_size_bytes = zone address space size (for alignment)
		// Store segment capacity for throttle calculation
		segment_capacity_bytes_ = zone_capacity_bytes * STRIPE_WIDTH;

		Config cfg;
		cfg.segment_bytes = zone_capacity_bytes;  // Single zone capacity (legacy)
		cfg.zone_size_bytes = zone_size_bytes;
		cfg.zone_capacity_bytes = zone_capacity_bytes;
		cfg.stripe_width = STRIPE_WIDTH;

		// Select evict/compactor policy based on cache_type
		std::unique_ptr<EvictPolicy> evictor;
		std::unique_ptr<EvictPolicy> compactor;
		double effective_valid_rate = valid_rate_threshold;
		bool score_low_valid_first = false;

		// IStream policy name (default: multi_hotcold_3)
		std::string istream_policy_name = "multi_hotcold_3";

		if (cache_type == "LOG_GREEDY_COST_BENEFIT_10" || cache_type == "LOG_GREEDY_COST_BENEFIT_11") {
			evictor = std::make_unique<CbEvictPolicy>(score_age_evict);
			compactor = std::make_unique<CbEvictPolicy>(score_warm_first);
			if (cache_type == "LOG_GREEDY_COST_BENEFIT_10") {
				effective_valid_rate = 0.6;
				score_low_valid_first = true;
			}
		}
			// LOG_GREEDY_COST_BENEFIT_11 uses valid_rate_threshold from parameter
		else if (cache_type == "LOG_GREEDY_COST_BENEFIT_10_WARM") {
			evictor = std::make_unique<CbEvictPolicy>(score_age_evict);
			compactor = std::make_unique<CbEvictPolicy>(score_warm_first);
			effective_valid_rate = 0.8;
			score_low_valid_first = false;
		} else if (cache_type == "LOG_GREEDY_COST_BENEFIT_HOT") {
			// Hot-first compaction: prefer recently created segments
			evictor = std::make_unique<CbEvictPolicy>(score_age_evict);
			compactor = std::make_unique<CbEvictPolicy>(score_hot_first);
			effective_valid_rate = 0.8;
			score_low_valid_first = false;
		} else if (cache_type == "LOG_GREEDY_COST_BENEFIT_COLD") {
			// Cold-first compaction: prefer older segments
			evictor = std::make_unique<CbEvictPolicy>(score_age_evict);
			compactor = std::make_unique<CbEvictPolicy>(score_cold_first);
			effective_valid_rate = 0.8;
			score_low_valid_first = false;
		} else if (cache_type == "LOG_SEPBIT_FIFO") {
			// SEPBIT with FIFO eviction and sqrt-age compaction
			evictor = std::make_unique<CbEvictPolicy>(score_age_evict);
			compactor = std::make_unique<CbEvictPolicy>(score_sepbit_age);
			effective_valid_rate = 0.8;
			istream_policy_name = "sepbit";
			score_low_valid_first = false;
		} else if (cache_type == "LOG_COST_BENEFIT") {
			evictor = std::make_unique<CbEvictPolicy>();
		} else {
			// Default: LOG_GREEDY
			evictor = std::make_unique<GreedyEvictPolicy>();
		}

		// Create IStream policy for stream separation (same as icache.cpp)
		// Align interval to segment boundary for proper FDP handle pingpong
		uint64_t segment_size_blocks = segment_capacity_bytes_ / block_size;
		set_stream_interval(static_cast<uint64_t>(cache_block_count), segment_size_blocks);
		IStream *input_stream_policy = createIstreamPolicy(istream_policy_name);

		SPDK_NOTICELOG("LogCacheAsync: cache_type=%s, valid_rate=%.2f, has_compactor=%d, istream=%p\n",
			       cache_type.c_str(), effective_valid_rate, compactor != nullptr, input_stream_policy);

		cache_ = std::make_unique<LogCache>(
			cold_capacity,
			cache_block_count,
			block_size,
			false,
			std::string{},
			std::string{},
			waf_path,
			std::move(evictor),
			&cfg,
			input_stream_policy,
			effective_valid_rate,
			std::move(compactor),
			0.0,
			score_low_valid_first,
			stat_log_path,
			device);
		cache_->set_stats_prefix("icache");
		cache_->set_async_mode(true);  // Disable sync eviction, use async wrapper

		// Initialize QoS throttle
		throttle_init();
	}

	// Check if key exists in cache
	bool exists(long key) { return cache_->exists(key); }

	// Get block offset for a segment and index (with striping support)
	uint64_t block_offset(const LogCacheSegment *seg, size_t idx) const {
		return seg->get_block_offset(idx, block_size_);
	}

	// Get mapping info for a key
	bool get_mapping(long key, LogCacheSegment **seg, size_t *idx) {
		if (!cache_->exists(key)) return false;
		// Access internal mapping through LogCache
		// We need to expose this or use a friend class
		return false; // TODO: Need to expose mapping from LogCache
	}

	// Accessors
	int block_size() const { return block_size_; }
	LogCache* cache() { return cache_.get(); }
	icache::SpdkCacheDevice* device() { return device_; }

	// GC/Evict state
	bool gc_in_progress() const { return gc_in_progress_; }
	bool evict_in_progress() const { return evict_in_progress_; }
	void set_gc_in_progress(bool v) { gc_in_progress_ = v; }
	void set_evict_in_progress(bool v) { evict_in_progress_ = v; }

	// Host write placement handle (toggle between 0 and 1) - delegated to LogCache
	int get_host_write_handle() const { return cache_->get_host_write_handle(); }

	// Pending writes waiting for GC/Evict
	std::list<CacheIo*>& pending_writes() { return pending_writes_; }

	// Check if free segments available
	bool need_gc_or_evict() { return cache_->is_cache_filled(); }

	// Free segment count for watermark checks
	size_t free_segment_count() const { return cache_->free_segment_count(); }

	// Returns true if free segments <= CRITICAL threshold (caller should block)
	bool is_free_critical() const { return cache_->free_segment_count() <= CRITICAL_FREE_SEGMENTS; }

	// Segment capacity for throttle calculation (freed capacity per GC/Evict)
	uint64_t segment_capacity() const { return segment_capacity_bytes_; }

	// 16KB Write Buffer - flush when this size is reached
	static constexpr size_t WRITE_BUFFER_SIZE = 16 * 1024;  // 16KB
	static constexpr uint64_t WRITE_BUFFER_TIMEOUT_US = 300;  // 300us
	// Set to false to bypass write buffer and write directly per-block
	static constexpr bool WRITE_BUFFER_ENABLED = true;
	static constexpr int MAX_IN_FLIGHT_FLUSHES = 4;  // Allow 4 concurrent zone writes

	struct BufferedBlock {
		CacheIo *io;
		uint32_t block_idx;
		uint64_t key;
		const uint8_t *buf;
		int stream_id;  // For FDP placement handle
	};

	// Add block to write buffer, flush when 1MB accumulated
	void buffer_add_block(CacheIo *io, uint32_t block_idx, uint64_t key, const uint8_t *buf, int stream_id = 0) {
		write_buffer_.push_back({io, block_idx, key, buf, stream_id});
		if (write_buffer_.size() * block_size_ >= WRITE_BUFFER_SIZE) {
			flush_write_buffer();
		}
	}

	// Flush write buffer (called when full or timeout)
	void flush_write_buffer();

	// Get write buffer
	std::vector<BufferedBlock>& write_buffer() { return write_buffer_; }
	bool buffer_empty() const { return write_buffer_.empty(); }

	// Timeout tracking
	void set_buffer_timer_active(bool active) { buffer_timer_active_ = active; }
	bool buffer_timer_active() const { return buffer_timer_active_; }

	// Flush pending tracking (for retry after GC/Evict)
	void set_flush_pending(bool pending) { flush_pending_ = pending; }
	bool flush_pending() const { return flush_pending_; }

	// WAF statistics
	void add_host_write_bytes(uint64_t bytes) {
		host_write_bytes_ += bytes;
		if (stats_logger_) {
			stats_logger_->add_host_write(bytes);
		}
		maybe_log_waf();
		update_stats_logger();
	}

	void add_gc_write_bytes(uint64_t bytes) {
		gc_write_bytes_ += bytes;
		if (stats_logger_) {
			stats_logger_->add_gc_write(bytes);
		}
	}

	// Cache device write tracking (host writes + GC writes)
	void add_cache_write_bytes(uint64_t bytes) {
		if (stats_logger_) {
			stats_logger_->add_cache_write(bytes);
		}
	}

	// Backend device write tracking (eviction writes)
	void add_backend_write_bytes(uint64_t bytes) {
		if (stats_logger_) {
			stats_logger_->add_backend_write(bytes);
		}
	}

	// Read tracking
	void add_cache_read_bytes(uint64_t bytes) {
		if (stats_logger_) {
			stats_logger_->add_cache_read(bytes);
		}
	}

	void add_backend_read_bytes(uint64_t bytes) {
		if (stats_logger_) {
			stats_logger_->add_backend_read(bytes);
		}
	}

	// Set NVMe controller for stats logger (for reading endurance group log page)
	void set_stats_nvme_ctrlr(struct spdk_nvme_ctrlr *ctrlr) {
		if (stats_logger_) {
			stats_logger_->set_nvme_ctrlr(ctrlr);
		}
	}

	// Start the stats logger (call from worker thread after channels set)
	void start_stats_logger() {
		if (stats_logger_) {
			// Register histogram print callback (~60 seconds)
			if (cache_) {
				stats_logger_->set_histogram_callback([this]() {
					cache_->print_histograms(false);  // print without reset (cumulative)
				});
			}
			stats_logger_->start();
		}
	}

	// Stop the stats logger
	void stop_stats_logger() {
		if (stats_logger_) {
			stats_logger_->stop();
		}
	}

	// Update stats logger with LogCache stats (call periodically)
	void update_stats_logger() {
		if (stats_logger_ && cache_) {
			stats_logger_->set_valid_blocks(cache_->get_valid_blocks());
			// write_hit, gc_victim, evict_victim are cumulative in LogCache
			// StatsLogger expects cumulative values too, so just set directly
			stats_logger_->stats().write_hit_count.store(cache_->get_write_hit_count(), std::memory_order_relaxed);
			stats_logger_->stats().gc_victim_blocks.store(cache_->get_compacted_blocks(), std::memory_order_relaxed);
			stats_logger_->stats().evict_victim_blocks.store(cache_->get_evicted_blocks(), std::memory_order_relaxed);
		}
	}

	double get_waf() const {
		if (host_write_bytes_ == 0) return 1.0;
		return static_cast<double>(host_write_bytes_ + gc_write_bytes_) / host_write_bytes_;
	}

	uint64_t host_write_bytes() const { return host_write_bytes_; }
	uint64_t gc_write_bytes() const { return gc_write_bytes_; }

private:
	static constexpr uint64_t WAF_LOG_INTERVAL = 10ULL * 1024 * 1024 * 1024;  // 10GB

	void maybe_log_waf() {
		if (host_write_bytes_ >= next_waf_log_threshold_) {
			next_waf_log_threshold_ += WAF_LOG_INTERVAL;
			if (waf_fp_) {
				// Format: host_write_bytes gc_write_bytes waf
				fprintf(waf_fp_, "%lu %lu %.4f\n",
					host_write_bytes_, gc_write_bytes_, get_waf());
				fflush(waf_fp_);
			}
		}
	}

	int block_size_;
	icache::SpdkCacheDevice *device_;
	std::unique_ptr<LogCache> cache_;
	bool gc_in_progress_;
	bool evict_in_progress_;
	std::list<CacheIo*> pending_writes_;
	// host_write_handle_ moved to LogCache for proper toggle timing
	uint64_t zone_size_bytes_;
	uint64_t segment_capacity_bytes_;  // zone_capacity * stripe_width

	// 16KB Write Buffer
	std::vector<BufferedBlock> write_buffer_;
	bool buffer_timer_active_ = false;
	bool flush_pending_ = false;  // Set when flush needs retry after GC/Evict

	// WAF statistics
	FILE *waf_fp_ = nullptr;
	uint64_t host_write_bytes_ = 0;
	uint64_t gc_write_bytes_ = 0;
	uint64_t next_waf_log_threshold_ = WAF_LOG_INTERVAL;

	// Stats Logger for detailed IO statistics
	std::string cache_type_;
	std::unique_ptr<StatsLogger> stats_logger_;

	//==========================================================================
	// QoS Throttle (free segment change based)
	// Adjusts host write rate based on free segment changes
	// - free_segs > 10: no throttle (100%)
	// - free_segs <= 10: throttle starts
	// - free_segs decreases: reduce by 10%
	// - free_segs increases AND > 7: increase by 10%
	// - free_segs <= 7: decrease only (no recovery)
	// - free_segs unchanged: maintain current ratio
	//==========================================================================
	struct Throttle {
		// Throttle state
		uint64_t interval_tsc = 0;           // 500ms interval in ticks
		uint64_t start_tsc = 0;              // Start of current interval
		size_t prev_free_segs = 0;           // Previous free segment count

		// Host IO tracking
		uint64_t blocks_submitted = 0;       // Host blocks submitted this interval
		uint64_t blocks_limit = 0;           // Limit for this interval (0 = no limit)

		// For logging
		enum class Level { NORMAL, LOW, CRITICAL };
		Level prev_level = Level::NORMAL;
	} throttle_;

	// Throttle constants
	static constexpr size_t THROTTLE_START_SEGS = 9;      // Start throttle at <= 9 free segs
	static constexpr size_t THROTTLE_CRITICAL_SEGS = 2;   // Critical - block all at <= 2
	static constexpr double THROTTLE_STEP_DOWN = 0.05;    // 5% decrease when free_segs drops
	static constexpr double THROTTLE_STEP_UP = 0.05;      // 5% increase when free_segs rises or unchanged
	static constexpr uint64_t THROTTLE_INTERVAL_MS = 500; // 500ms interval

public:
	// Initialize throttle (call after device ready)
	void throttle_init() {
		throttle_.interval_tsc = THROTTLE_INTERVAL_MS * spdk_get_ticks_hz() / 1000;
		throttle_.start_tsc = spdk_get_ticks();
		throttle_.prev_free_segs = free_segment_count();
		throttle_.blocks_submitted = 0;
		throttle_.blocks_limit = 0;  // 0 = no limit
	}

	// Record GC completion (for logging only now)
	void throttle_gc_complete(uint64_t gc_bytes, uint64_t gc_ticks) {
		(void)gc_bytes;
		(void)gc_ticks;
		// No longer used for throttle calculation
	}

	// Update throttle limit based on free segment changes (called every 500ms)
	void throttle_update() {
		auto &t = throttle_;
		uint64_t now = spdk_get_ticks();

		if (t.start_tsc == 0) {
			t.start_tsc = now;
			t.prev_free_segs = free_segment_count();
			return;
		}

		if (now - t.start_tsc < t.interval_tsc) {
			return;  // Not time yet
		}

		size_t current_free = free_segment_count();
		size_t prev_free = t.prev_free_segs;
		uint64_t current_perf = t.blocks_submitted > 0 ? t.blocks_submitted : 1;

		// Determine level for logging
		Throttle::Level cur_level;
		if (current_free <= THROTTLE_CRITICAL_SEGS) cur_level = Throttle::Level::CRITICAL;
		else if (current_free <= THROTTLE_START_SEGS) cur_level = Throttle::Level::LOW;
		else cur_level = Throttle::Level::NORMAL;

		// Log level changes
		if (cur_level != t.prev_level) {
			static const char* level_names[] = {"NORMAL", "LOW", "CRITICAL"};
			SPDK_NOTICELOG("THROTTLE: %s -> %s (free_segs=%zu, limit=%lu)\n",
				       level_names[static_cast<int>(t.prev_level)],
				       level_names[static_cast<int>(cur_level)],
				       current_free, t.blocks_limit);
			t.prev_level = cur_level;
		}

		// Apply throttle policy based on free segment change
		if (current_free > THROTTLE_START_SEGS) {
			// Above threshold (>9): no throttle
			if (t.blocks_limit > 0) {
				SPDK_NOTICELOG("THROTTLE: Recovered to unlimited (free_segs=%zu)\n", current_free);
			}
			t.blocks_limit = 0;  // 0 = no limit
		} else {
			// In throttle zone (<=9)
			if (current_free < prev_free) {
				// Free segments decreased -> next limit = current_perf * 0.95
				t.blocks_limit = static_cast<uint64_t>(current_perf * (1.0 - THROTTLE_STEP_DOWN));
				if (t.blocks_limit < 1) t.blocks_limit = 1;
				SPDK_NOTICELOG("THROTTLE: Decreased limit to %lu (was %lu, free_segs=%zu->%zu)\n",
					       t.blocks_limit, current_perf, prev_free, current_free);
			} else {
				// Free segments increased or unchanged -> next limit = current_perf * 1.03
				t.blocks_limit = static_cast<uint64_t>(current_perf * (1.0 + THROTTLE_STEP_UP));
				// No upper cap here - limit removed when free_segs > 9
				SPDK_NOTICELOG("THROTTLE: Increased limit to %lu (was %lu, free_segs=%zu->%zu)\n",
					       t.blocks_limit, current_perf, prev_free, current_free);
			}
		}

		// Reset for next interval
		t.prev_free_segs = current_free;
		t.start_tsc = now;
		t.blocks_submitted = 0;
	}

	// Check if host write should be throttled
	bool throttle_should_block() {
		throttle_update();

		size_t free_segs = free_segment_count();

		// Critical level (<=2) - block all host writes
		if (free_segs <= THROTTLE_CRITICAL_SEGS) {
			return true;
		}

		// No throttle if above threshold
		if (free_segs > THROTTLE_START_SEGS) {
			return false;
		}

		// No throttle if GC/Evict not running
		if (!gc_in_progress_ && !evict_in_progress_) {
			return false;
		}

		// No limit set yet (first interval in throttle zone)
		if (throttle_.blocks_limit == 0) {
			return false;
		}

		// Block if submitted >= limit
		return throttle_.blocks_submitted >= throttle_.blocks_limit;
	}

	// Record host write block submission
	void throttle_record_host_write(size_t num_blocks) {
		throttle_.blocks_submitted += num_blocks;
	}

	// Get current throttle state for debugging
	uint64_t throttle_limit() const { return throttle_.blocks_limit; }
	size_t throttle_submitted() const { return throttle_.blocks_submitted; }
};

//==============================================================================
// Forward declarations for write buffer
//==============================================================================
static int write_buffer_timeout_poller(void *arg);
static void start_gc_or_evict(log_cache_ctx *ctx, std::function<void(int)> on_complete);
static void process_pending_writes(log_cache_ctx *ctx);
static void cache_io_complete(CacheIo *io, int status);

//==============================================================================
// Write buffer flush context (scatter-gather, no memcpy)
// Supports split writes at zone capacity boundary
//==============================================================================
struct WriteBufferFlushCtx {
	std::vector<LogCacheAsync::BufferedBlock> blocks;
	std::vector<uint64_t> cache_offsets;
	std::vector<int> stream_ids;  // For FDP placement handle
	std::vector<long> keys;       // Keys for pending write completion
	LogCache *cache = nullptr;    // For complete_block_writes
	// Track IOs and their block counts in this flush
	std::unordered_map<CacheIo*, size_t> io_block_counts;
	// For split writes: track outstanding write count
	std::atomic<int> outstanding_writes{0};
	std::atomic<int> first_error{0};
};

// Context for individual zone write (part of split write)
struct ZoneWriteCtx {
	WriteBufferFlushCtx *parent;
	struct iovec *iovs;
	int iovcnt;
	size_t write_size;  // bytes for this write
	int placement_handle;  // FDP placement handle for this write
};

// Outstanding IO tracking
static std::atomic<uint64_t> g_outstanding_bytes{0};

// TEST: Skip zone write and complete immediately (for measuring icache overhead only)
static constexpr bool SKIP_ZONE_WRITE_FOR_TEST = false;

static void write_buffer_flush_done(void *cb_arg, int status);
static void zone_write_done(void *cb_arg, int status);

// Zone write completion - called for each split write
static void zone_write_done(void *cb_arg, int status)
{
	auto *zctx = static_cast<ZoneWriteCtx*>(cb_arg);
	auto *flush_ctx = zctx->parent;

	// Update outstanding bytes
	g_outstanding_bytes -= zctx->write_size;

	// Free this zone's iovec
	if (zctx->iovs) {
		free(zctx->iovs);
	}

	// Track first error
	if (status != 0 && flush_ctx->first_error == 0) {
		SPDK_ERRLOG("ZONE_WRITE_DONE: error status=%d\n", status);
		flush_ctx->first_error = status;
	}

	// Decrement outstanding writes
	int remaining = --flush_ctx->outstanding_writes;
	delete zctx;

	// If all writes done, complete the flush
	if (remaining == 0) {
		write_buffer_flush_done(flush_ctx, flush_ctx->first_error.load());
	}
}

// LogCacheAsync::flush_write_buffer implementation (scatter-gather, no memcpy)
// Handles zone capacity boundary by splitting writes
void LogCacheAsync::flush_write_buffer()
{
	if (write_buffer_.empty()) {
		return;
	}

	// Only block if GC/Evict in progress AND segments critically low
	if ((gc_in_progress_ || evict_in_progress_) &&
	    free_segment_count() <= CRITICAL_FREE_SEGMENTS) {
		flush_pending_ = true;
		return;
	}

	uint32_t block_size = block_size_;
	size_t total_blocks = write_buffer_.size();


	// Allocate flush context
	auto *flush_ctx = new (std::nothrow) WriteBufferFlushCtx();
	if (!flush_ctx) {
		SPDK_ERRLOG("Failed to allocate WriteBufferFlushCtx\n");
		return;
	}

	// Get metadata first (NO iovec allocation yet - will do per-group)
	// Store buffer pointers for later iovec setup
	static uint64_t flush_block_count = 0;
	static uint64_t block_start_tsc = 0;
	std::vector<const uint8_t*> buf_ptrs;
	size_t success_count = 0;
	bool partial_failure = false;
	for (size_t i = 0; i < write_buffer_.size(); i++) {
		auto &blk = write_buffer_[i];
		uint64_t cache_offset;
		int stream_id = 0;
		bool segment_full = false;
		if (!cache_->append_block_metadata(0, static_cast<long>(blk.key),
						   static_cast<int>(block_size), &cache_offset, &stream_id, &segment_full)) {
			// No free segments - need GC/Evict
			// if (flush_block_count == 0) {
			// 	block_start_tsc = spdk_get_ticks();
			// }
			// flush_block_count++;
			// if (flush_block_count == 1 || flush_block_count % 10000 == 0) {
			// 	SPDK_WARNLOG("BLOCKED: flush_write_buffer waiting for GC (blocked %lu times, success_count=%zu)\n",
			// 		     flush_block_count, success_count);
			// }
			partial_failure = true;
			flush_pending_ = true;
			break;  // Exit loop, but continue to write successful blocks
		}
		// Note: toggle is now done inside append_block_metadata when segment becomes full
		flush_ctx->cache_offsets.push_back(cache_offset);
		flush_ctx->stream_ids.push_back(stream_id);
		flush_ctx->keys.push_back(static_cast<long>(blk.key));
		buf_ptrs.push_back(blk.buf);
		success_count++;
	}

	// If no blocks succeeded, just return and wait for GC
	if (success_count == 0) {
		delete flush_ctx;
		return;
	}

	// Update total_blocks to reflect only successful blocks
	total_blocks = success_count;

	// Store cache pointer for completion
	flush_ctx->cache = cache_.get();

	// // Log if we were blocked and now succeeded (full success only)
	// if (flush_block_count > 0 && !partial_failure) {
	// 	uint64_t blocked_us = (spdk_get_ticks() - block_start_tsc) * 1000000 / spdk_get_ticks_hz();
	// 	SPDK_NOTICELOG("UNBLOCKED: flush_write_buffer succeeded after %lu blocks, %lu us blocked\n",
	// 		       flush_block_count, blocked_us);
	// 	flush_block_count = 0;
	// }

	// Copy successful blocks to flush_ctx, keep failed ones in buffer for retry
	for (size_t i = 0; i < success_count; i++) {
		flush_ctx->blocks.push_back(std::move(write_buffer_[i]));
	}
	// Remove successful blocks from buffer (failed ones remain for retry)
	write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + success_count);

	for (auto &blk : flush_ctx->blocks) {
		if (blk.io) {
			flush_ctx->io_block_counts[blk.io]++;
		}
	}

	// Track host write bytes for WAF calculation
	add_host_write_bytes(total_blocks * block_size);

	// Get zone info from device
	uint64_t zone_size = device_->zone_size_bytes();
	uint64_t zone_capacity = device_->zone_capacity_bytes();

	// Group consecutive offsets within same zone capacity
	// Each group becomes a separate write
	struct WriteGroup {
		size_t start_idx;
		size_t count;
		uint64_t first_offset;
		int stream_id;  // For FDP placement handle
	};
	std::vector<WriteGroup> groups;

	size_t i = 0;
	while (i < total_blocks) {
		WriteGroup group;
		group.start_idx = i;
		group.first_offset = flush_ctx->cache_offsets[i];
		group.stream_id = flush_ctx->stream_ids[i];
		group.count = 1;

		// Calculate zone boundary for this offset
		uint64_t zone_id = group.first_offset / zone_size;
		uint64_t zone_start = zone_id * zone_size;
		uint64_t zone_end = zone_start + zone_capacity;

		// Add consecutive blocks that stay within zone capacity AND same stream_id
		while (i + group.count < total_blocks) {
			uint64_t expected_next = group.first_offset + group.count * block_size;
			uint64_t actual_next = flush_ctx->cache_offsets[i + group.count];
			int next_stream_id = flush_ctx->stream_ids[i + group.count];

			// Check if consecutive
			if (actual_next != expected_next) {
				break;  // Not consecutive (probably zone transition)
			}

			// Check if next block would exceed zone capacity
			if (actual_next + block_size > zone_end) {
				break;  // Would exceed zone capacity
			}

#if FDP
			// For FDP: break if stream_id changes (different placement handle)
			if (next_stream_id != group.stream_id) {
				break;
			}
#endif

			group.count++;
		}

		groups.push_back(group);
		i += group.count;
	}


	// Set outstanding writes count
	flush_ctx->outstanding_writes = static_cast<int>(groups.size());

	// TEST: Skip zone write and complete immediately
	if (SKIP_ZONE_WRITE_FOR_TEST) {
		write_buffer_flush_done(flush_ctx, 0);
		return;
	}

	// Issue writes for each group
	for (auto &group : groups) {
		// Allocate iovec for this group
		struct iovec *iovs = static_cast<struct iovec*>(calloc(group.count, sizeof(struct iovec)));
		if (!iovs) {
			SPDK_ERRLOG("Failed to allocate iovec for group\n");
			flush_ctx->first_error = -ENOMEM;
			if (--flush_ctx->outstanding_writes == 0) {
				write_buffer_flush_done(flush_ctx, -ENOMEM);
			}
			continue;
		}

		// Setup iovecs for this group
		for (size_t j = 0; j < group.count; j++) {
			iovs[j].iov_base = const_cast<uint8_t*>(buf_ptrs[group.start_idx + j]);
			iovs[j].iov_len = block_size;
		}

		// Create zone write context
		size_t group_len = group.count * block_size;
		// Use the stream_id recorded per-block (host_write_handle at append time)
		int placement_handle = group.stream_id;
		auto *zctx = new (std::nothrow) ZoneWriteCtx{flush_ctx, iovs, static_cast<int>(group.count), group_len, placement_handle};
		if (!zctx) {
			free(iovs);
			flush_ctx->first_error = -ENOMEM;
			if (--flush_ctx->outstanding_writes == 0) {
				write_buffer_flush_done(flush_ctx, -ENOMEM);
			}
			continue;
		}

		// Track outstanding IO
		g_outstanding_bytes.fetch_add(group_len);

#if OFFSET_DEBUG
		for (size_t j = 0; j < group.count; j++) {
			size_t blk_idx = group.start_idx + j;
			SPDK_NOTICELOG("BUFFER_WRITE_SUBMIT: key=%lu cache_offset=0x%lx\n",
				       flush_ctx->keys[blk_idx], flush_ctx->cache_offsets[blk_idx]);
		}
#endif
		int rc = device_->writev_cache_async(group.first_offset, iovs, zctx->iovcnt,
						     group_len, zone_write_done, zctx, placement_handle);
		if (rc == 0) {
			// Track cache write bytes for stats
			add_cache_write_bytes(group_len);
		} else {
			SPDK_ERRLOG("writev_cache_async failed for group: %d\n", rc);
			g_outstanding_bytes -= group_len;  // Rollback on error
			free(iovs);
			delete zctx;
			flush_ctx->first_error = rc;
			if (--flush_ctx->outstanding_writes == 0) {
				write_buffer_flush_done(flush_ctx, rc);
			}
		}
	}
}

static void write_buffer_flush_done(void *cb_arg, int status)
{
	auto *flush_ctx = static_cast<WriteBufferFlushCtx*>(cb_arg);

	// Mark all blocks as write complete (clear pending)
	if (flush_ctx->cache && !flush_ctx->keys.empty()) {
		flush_ctx->cache->complete_block_writes(flush_ctx->keys);
	}

	// Update flushed block counts and complete IOs that are fully flushed
	for (auto &pair : flush_ctx->io_block_counts) {
		CacheIo *io = pair.first;
		size_t blocks_in_this_flush = pair.second;

		// Track error status in the IO
		if (status != 0 && io->last_status == 0) {
			io->last_status = status;
		}

		// Atomically add flushed blocks
		size_t new_flushed = io->flushed_blocks.fetch_add(blocks_in_this_flush) + blocks_in_this_flush;


		// Only complete when all blocks for this IO have been flushed
		if (new_flushed >= io->total_blocks) {
			io->state = CacheIoState::HOST_WRITE_DONE;
			cache_io_complete(io, io->last_status);
		}
	}

	delete flush_ctx;
}

// Timeout poller for write buffer
static int write_buffer_timeout_poller(void *arg)
{
	auto *ctx = static_cast<log_cache_ctx*>(arg);
	if (!ctx || !ctx->cache) {
		return SPDK_POLLER_IDLE;
	}

	LogCacheAsync *cache = ctx->cache.get();

	// Proactive GC/Evict: trigger when needed (but don't block flush!)
	if (cache->need_gc_or_evict() &&
	    !cache->gc_in_progress() && !cache->evict_in_progress()) {
		start_gc_or_evict(ctx, [ctx](int status) {
			process_pending_writes(ctx);
		});
	}
	if (!cache->buffer_empty()) {
		cache->flush_write_buffer();
	}
	process_pending_writes(ctx);

	return SPDK_POLLER_BUSY;
}

//==============================================================================
// Completion callbacks
//==============================================================================
static void cache_io_complete(CacheIo *io, int status)
{
	auto cb = io->cb_fn;
	void *arg = io->cb_arg;
	delete io;
	if (cb) {
		cb(arg, status);
	}
}

static void gc_io_complete(GcIo *io, int status)
{
	// // Calculate and log GC timing
	// if (io->start_ticks > 0) {
	// 	uint64_t elapsed_ticks = spdk_get_ticks() - io->start_ticks;
	// 	// Use segment capacity for throttle (freed space, not copied bytes)
	// 	io->ctx->cache->throttle_gc_complete(io->ctx->cache->segment_capacity(), elapsed_ticks);

	// 	double ticks_hz = spdk_get_ticks_hz();
	// 	double elapsed_sec = elapsed_ticks / ticks_hz;
	// 	double read_sec = io->read_ticks / ticks_hz;
	// 	double write_sec = io->write_ticks / ticks_hz;
	// 	double mb = io->total_gc_bytes / 1024.0 / 1024.0;
	// 	double throughput_mbs = (elapsed_sec > 0) ? (mb / elapsed_sec) : 0;
	// 	double read_mbs = (read_sec > 0) ? (mb / read_sec) : 0;
	// 	double write_mbs = (write_sec > 0) ? (mb / write_sec) : 0;

	// 	SPDK_NOTICELOG("GC complete: victim_seg=%p, %.1f MB, total=%.2fs (%.0f MB/s), "
	// 		       "read=%.2fs (%.0f MB/s), write=%.2fs (%.0f MB/s), status=%d\n",
	// 		       (void*)io->prepare_result.victim_seg, mb, elapsed_sec, throughput_mbs,
	// 		       read_sec, read_mbs, write_sec, write_mbs, status);
	// }

	auto on_complete = std::move(io->on_complete);
	auto victim_seg = io->prepare_result.victim_seg;
	(void)victim_seg;  // For potential future debugging
	delete io;
	if (on_complete) {
		on_complete(status);
	}
}

static void evict_io_complete(EvictIo *io, int status)
{
	// // Calculate and log segment evict time
	// uint64_t elapsed_ticks = spdk_get_ticks() - io->start_ticks;
	// uint64_t elapsed_us = elapsed_ticks * 1000000 / spdk_get_ticks_hz();
	// double elapsed_sec = elapsed_us / 1000000.0;
	// double mb = io->total_bytes / (1024.0 * 1024.0);
	// double throughput_mbs = (elapsed_sec > 0) ? (mb / elapsed_sec) : 0;

	// double read_sec = io->read_total_us / 1000000.0;
	// double write_sec = io->write_total_us / 1000000.0;
	// double read_mbs = (read_sec > 0) ? (mb / read_sec) : 0;
	// double write_mbs = (write_sec > 0) ? (mb / write_sec) : 0;

	// // Record evict timing for QoS throttle (use segment capacity = freed space)
	// io->ctx->cache->throttle_gc_complete(io->ctx->cache->segment_capacity(), elapsed_ticks);

	// SPDK_NOTICELOG("Evict complete: victim_seg=%p, %.1f MB, total=%.2fs (%.0f MB/s), "
	// 	       "read=%.2fs (%.0f MB/s), write=%.2fs (%.0f MB/s), status=%d\n",
	// 	       (void*)io->prepare_result.victim_seg, mb, elapsed_sec, throughput_mbs,
	// 	       read_sec, read_mbs, write_sec, write_mbs, status);

	auto on_complete = std::move(io->on_complete);
	auto victim_seg = io->prepare_result.victim_seg;
	(void)victim_seg;  // For potential future debugging
	delete io;
	SPDK_NOTICELOG("evict_io_complete: calling on_complete=%p\n", (void*)&on_complete);
	if (on_complete) {
		on_complete(status);
	}
	SPDK_NOTICELOG("evict_io_complete: done\n");
}

//==============================================================================
// Helper: Get buffer pointer for block index from iov array
// Returns pointer into the iov buffer (already hugepage-backed from vbdev layer)
//==============================================================================
static uint8_t *get_iov_buf_for_block(const struct iovec *iovs, int iovcnt,
				      uint32_t block_size, size_t block_idx)
{
	size_t offset = block_idx * block_size;
	size_t pos = 0;
	for (int i = 0; i < iovcnt; ++i) {
		if (offset < pos + iovs[i].iov_len) {
			return static_cast<uint8_t *>(iovs[i].iov_base) + (offset - pos);
		}
		pos += iovs[i].iov_len;
	}
	return nullptr;
}

//==============================================================================
// Host Read State Machine
//==============================================================================
struct ReadBlockCtx {
	CacheIo *io;
	size_t block_idx;
	uint64_t key;           // LBA key for error logging
	uint64_t read_offset;   // Actual read offset (cache or backend)
	bool is_cache;          // true = read from cache, false = read from backend
};

static void host_read_block_done(void *cb_arg, int status);

static void host_read_next_block(CacheIo *io)
{

	if (io->last_status != 0) {
		io->state = CacheIoState::HOST_READ_DONE;
		cache_io_complete(io, io->last_status);
		return;
	}

	if (io->current_block_idx >= io->total_blocks) {
		// All blocks read - no copy needed, data is already in iovs
		io->state = CacheIoState::HOST_READ_DONE;
		cache_io_complete(io, 0);
		return;
	}

	uint32_t block_size = io->ctx->block_size;
	uint64_t key = io->lba + io->current_block_idx;

	// Get buffer directly from iov (hugepage-backed)
	uint8_t *dest = get_iov_buf_for_block(io->iovs, io->iovcnt, block_size, io->current_block_idx);
	if (!dest) {
		SPDK_ERRLOG("READ_NEXT: failed to get iov buf for blk=%lu\n", io->current_block_idx);
		cache_io_complete(io, -EINVAL);
		return;
	}

	auto *read_ctx = new (std::nothrow) ReadBlockCtx{io, io->current_block_idx, key, 0, false};
	if (!read_ctx) {
		SPDK_ERRLOG("READ_NEXT: failed to alloc ReadBlockCtx\n");
		cache_io_complete(io, -ENOMEM);
		return;
	}

	LogCacheAsync *cache = io->ctx->cache.get();

	// Check if in cache or backend
	if (cache->exists(static_cast<long>(key))) {
		// Read from cache async
		uint64_t cache_offset;
		if (!cache->cache()->get_cache_location(static_cast<long>(key), &cache_offset)) {
			SPDK_ERRLOG("READ_NEXT: get_cache_location FAILED for key=%lu (exists=true but no location!)\n", key);
			delete read_ctx;
			cache_io_complete(io, -EIO);
			return;
		}
		read_ctx->read_offset = cache_offset;
		read_ctx->is_cache = true;
		io->state = CacheIoState::HOST_READ_CACHE_READ;
		int rc = cache->device()->read_cache_async(cache_offset, dest, block_size,
							   host_read_block_done, read_ctx);
		if (rc != 0) {
			SPDK_ERRLOG("READ_NEXT: read_cache_async submit failed key=%lu offset=0x%lx rc=%d\n", key, cache_offset, rc);
			delete read_ctx;
			cache_io_complete(io, rc);
		}
	} else {
		// Read from backend
		uint64_t backend_offset = key * block_size;
		read_ctx->read_offset = backend_offset;
		read_ctx->is_cache = false;
		io->state = CacheIoState::HOST_READ_BACKEND_READ;
		int rc = cache->device()->read_backend_async(backend_offset, dest, block_size,
							     host_read_block_done, read_ctx);
		if (rc != 0) {
			SPDK_ERRLOG("READ_NEXT: read_backend_async submit failed key=%lu offset=0x%lx rc=%d\n", key, backend_offset, rc);
			delete read_ctx;
			cache_io_complete(io, rc);
		}
	}
}

static void host_read_block_done(void *cb_arg, int status)
{
	auto *ctx = static_cast<ReadBlockCtx *>(cb_arg);
	CacheIo *io = ctx->io;
	uint64_t blk_idx = ctx->block_idx;
	uint64_t key = ctx->key;
	uint64_t read_offset = ctx->read_offset;
	bool is_cache = ctx->is_cache;
	delete ctx;

	if (status != 0) {
		SPDK_ERRLOG("READ_DONE: FAILED! key=%lu blk_idx=%lu %s offset=0x%lx status=%d\n",
			    key, blk_idx, is_cache ? "CACHE" : "BACKEND", read_offset, status);
		io->last_status = status;
	}

	// Parallel completion: increment counter and check if all done
	size_t done = ++io->parallel_reads_done;
	if (done >= io->total_blocks) {
		io->state = CacheIoState::HOST_READ_DONE;
		cache_io_complete(io, io->last_status);
	}
}

static void cache_io_run_host_read(CacheIo *io)
{
	if (!io->ctx || !io->iovs || io->iovcnt <= 0 || io->total_len == 0) {
		cache_io_complete(io, -EINVAL);
		return;
	}
	if (io->total_len % io->ctx->block_size != 0) {
		cache_io_complete(io, -EINVAL);
		return;
	}

	io->state = CacheIoState::HOST_READ_SUBMIT;
	uint32_t block_size = io->ctx->block_size;
	io->total_blocks = io->total_len / block_size;
	io->parallel_reads_done.store(0);
	io->last_status = 0;

	if (io->total_blocks == 0) {
		cache_io_complete(io, 0);
		return;
	}

	LogCacheAsync *cache = io->ctx->cache.get();

	// Submit all blocks in parallel
	for (size_t blk_idx = 0; blk_idx < io->total_blocks; blk_idx++) {
		uint64_t key = io->lba + blk_idx;

		uint8_t *dest = get_iov_buf_for_block(io->iovs, io->iovcnt, block_size, blk_idx);
		if (!dest) {
			SPDK_ERRLOG("READ_PARALLEL: failed to get iov buf for blk=%zu\n", blk_idx);
			// Count as done with error
			io->last_status = -EINVAL;
			size_t done = ++io->parallel_reads_done;
			if (done >= io->total_blocks) {
				cache_io_complete(io, io->last_status);
				return;
			}
			continue;
		}

		auto *read_ctx = new (std::nothrow) ReadBlockCtx{io, blk_idx, key, 0, false};
		if (!read_ctx) {
			io->last_status = -ENOMEM;
			size_t done = ++io->parallel_reads_done;
			if (done >= io->total_blocks) {
				cache_io_complete(io, io->last_status);
				return;
			}
			continue;
		}

		int rc = 0;
		if (cache->exists(static_cast<long>(key))) {
			uint64_t cache_offset;
			if (!cache->cache()->get_cache_location(static_cast<long>(key), &cache_offset)) {
				SPDK_ERRLOG("READ_PARALLEL: get_cache_location FAILED key=%lu (exists=true but no location!)\n", key);
				delete read_ctx;
				io->last_status = -EIO;
				size_t done = ++io->parallel_reads_done;
				if (done >= io->total_blocks) {
					cache_io_complete(io, io->last_status);
					return;
				}
				continue;
			}
			read_ctx->read_offset = cache_offset;
			read_ctx->is_cache = true;
			rc = cache->device()->read_cache_async(cache_offset, dest, block_size,
							       host_read_block_done, read_ctx);
		} else {
			uint64_t backend_offset = key * block_size;
			read_ctx->read_offset = backend_offset;
			read_ctx->is_cache = false;
			rc = cache->device()->read_backend_async(backend_offset, dest, block_size,
								 host_read_block_done, read_ctx);
		}

		if (rc != 0) {
			SPDK_ERRLOG("READ_PARALLEL: submit failed key=%lu %s offset=0x%lx rc=%d\n",
				    key, read_ctx->is_cache ? "CACHE" : "BACKEND", read_ctx->read_offset, rc);
			delete read_ctx;
			io->last_status = rc;
			size_t done = ++io->parallel_reads_done;
			if (done >= io->total_blocks) {
				cache_io_complete(io, io->last_status);
				return;
			}
		}
	}
}

//==============================================================================
// Host Write State Machine
//==============================================================================
static void host_write_next_block(CacheIo *io);
static void cache_io_run_host_write(CacheIo *io);

static void host_write_block_done(void *cb_arg, int status)
{
	CacheIo *io = static_cast<CacheIo *>(cb_arg);


	if (status != 0 && io->last_status == 0) {
		SPDK_ERRLOG("WRITE_BLK_DONE: error status=%d\n", status);
		io->last_status = status;
	}

	io->current_block_idx++;

	LogCacheAsync *cache = io->ctx->cache.get();

	// Check if we need GC/Evict after this write
	if (cache->need_gc_or_evict() && !cache->gc_in_progress() && !cache->evict_in_progress()) {
		// Need to trigger GC/Evict
		io->state = CacheIoState::HOST_WRITE_WAIT_GC_EVICT;
		cache->pending_writes().push_back(io);
		static uint64_t wait_gc_count = 0;
		if (++wait_gc_count % 1000 == 1) {
			SPDK_WARNLOG("BLOCKED: host_write waiting for GC, pending_writes=%zu (blocked %lu times)\n",
			       cache->pending_writes().size(), wait_gc_count);
		}

		start_gc_or_evict(io->ctx, [ctx = io->ctx](int status) {
			process_pending_writes(ctx);
		});
		return;
	}

	host_write_next_block(io);
}

static void host_write_next_block(CacheIo *io)
{
	log_cache_ctx *ctx = io->ctx;
	LogCacheAsync *cache = ctx->cache.get();


	if (io->last_status != 0) {
		// If blocks have already been buffered, don't complete here
		// The IO will be completed when the buffer is flushed
		if (io->current_block_idx > 0) {
			// Update total_blocks to reflect only buffered blocks
			// This ensures IO completes when flushed_blocks reaches this count
			io->total_blocks = io->current_block_idx;
			return;
		}
		io->state = CacheIoState::HOST_WRITE_DONE;
		cache_io_complete(io, io->last_status);
		return;
	}

	if (io->current_block_idx >= io->total_blocks) {
		// All blocks done
		if (LogCacheAsync::WRITE_BUFFER_ENABLED) {
			// Buffer mode: completion will be called after flush_write_buffer completes
			return;
		} else {
			// Direct mode: complete now
			io->state = CacheIoState::HOST_WRITE_DONE;
			cache_io_complete(io, io->last_status);
			return;
		}
	}

	uint32_t block_size = io->ctx->block_size;
	uint64_t key = io->lba + io->current_block_idx;

	// Get buffer directly from iov (hugepage-backed)
	const uint8_t *src = get_iov_buf_for_block(io->iovs, io->iovcnt, block_size, io->current_block_idx);
	if (!src) {
		SPDK_ERRLOG("WRITE_NEXT: failed to get iov buf for blk=%lu\n", io->current_block_idx);
		// If blocks have already been buffered, let flush complete with error
		if (io->current_block_idx > 0) {
			io->last_status = -EINVAL;
			io->total_blocks = io->current_block_idx;
			return;
		}
		cache_io_complete(io, -EINVAL);
		return;
	}

	if (LogCacheAsync::WRITE_BUFFER_ENABLED) {
		// Add to write buffer (batched writes)
		cache->buffer_add_block(io, io->current_block_idx, key, src);

		// Move to next block
		io->current_block_idx++;
		host_write_next_block(io);
	} else {
		// Direct write path - bypass buffer
		uint64_t cache_offset;
		int stream_id = 0;
		bool segment_full = false;
		if (!cache->cache()->append_block_metadata(0, static_cast<long>(key),
							   static_cast<int>(block_size), &cache_offset, &stream_id, &segment_full)) {
			// No free segments - check if we should block or just trigger GC
			size_t critical = cache->is_free_critical();

			if (true) {
				// Critical - block and wait for GC
				static uint64_t direct_gc_wait_count = 0;
				if (++direct_gc_wait_count % 1000 == 1) {
					SPDK_WARNLOG("WRITE_DIRECT: no free segment for key=%lu, blocking for GC (waited %lu times)\n",
						     key, direct_gc_wait_count);
				}

				io->state = CacheIoState::HOST_WRITE_WAIT_GC_EVICT;
				cache->pending_writes().push_back(io);

				if (!cache->gc_in_progress() && !cache->evict_in_progress()) {
					start_gc_or_evict(ctx, [ctx](int status) {
						process_pending_writes(ctx);
					});
				}
				return;
			}

			SPDK_WARNLOG("WRITE_DIRECT: no free segment for key=%lu", key);
			io->last_status = -ENOSPC;
			cache_io_complete(io, -ENOSPC);
			return;
		}
		// Note: toggle is now done inside append_block_metadata when segment becomes full
		// Not critical but low - trigger GC in background and return error
		if (!cache->gc_in_progress() && !cache->evict_in_progress()) {
			start_gc_or_evict(ctx, nullptr);
		}

		// Track host write for WAF
		cache->add_host_write_bytes(block_size);

		// Use the stream_id returned from append_block_metadata (host_write_handle at append time)
		int placement_handle = stream_id;

		// Issue async write with FDP placement handle using writev
		struct iovec iov;
		iov.iov_base = reinterpret_cast<void*>(const_cast<uint8_t*>(src));
		iov.iov_len = block_size;

#if OFFSET_DEBUG
		SPDK_NOTICELOG("WRITE_SUBMIT: key=%lu cache_offset=0x%lx\n", key, cache_offset);
#endif
		int rc = cache->device()->writev_cache_async(cache_offset, &iov, 1, block_size,
							     [](void *cb_arg, int status) {
								     CacheIo *io = static_cast<CacheIo *>(cb_arg);
								     // Mark block write complete (clear pending)
								     long completed_key = static_cast<long>(io->lba + io->current_block_idx);
								     io->ctx->cache->cache()->complete_block_write(completed_key);
#if OFFSET_DEBUG
								     SPDK_NOTICELOG("WRITE_DONE: key=%ld status=%d\n", completed_key, status);
#endif
								     if (status != 0) {
									     io->last_status = status;
								     }
								     io->current_block_idx++;
								     host_write_next_block(io);
							     }, io, placement_handle);
		if (rc == 0) {
			// Track cache write bytes for stats
			cache->add_cache_write_bytes(block_size);
		} else {
			SPDK_ERRLOG("WRITE_DIRECT: writev_cache_async failed rc=%d\n", rc);
			// Clear pending even on error to avoid stuck state
			cache->cache()->complete_block_write(static_cast<long>(key));
			io->last_status = rc;
			cache_io_complete(io, rc);
			return;
		}
	}
}

static void cache_io_run_host_write(CacheIo *io)
{
	if (!io->ctx || !io->iovs || io->iovcnt <= 0 || io->total_len == 0) {
		cache_io_complete(io, -EINVAL);
		return;
	}
	if (io->total_len % io->ctx->block_size != 0) {
		cache_io_complete(io, -EINVAL);
		return;
	}

	LogCacheAsync *cache = io->ctx->cache.get();

	// Initialize IO state (needed before any throttle check)
	io->total_blocks = io->total_len / io->ctx->block_size;
	io->current_block_idx = 0;
	io->completed_blocks = 0;
	io->last_status = 0;

	// Block host writes when free segments are critically low
	if (cache->is_free_critical() || cache->throttle_should_block()) {
		io->state = CacheIoState::HOST_WRITE_WAIT_GC_EVICT;
		cache->pending_writes().push_back(io);

		// Always trigger GC if not already running
		if (!cache->gc_in_progress() && !cache->evict_in_progress()) {
			start_gc_or_evict(io->ctx, [ctx = io->ctx](int status) {
				process_pending_writes(ctx);
			});
		}
		return;
	}
	if (!cache->gc_in_progress() && !cache->evict_in_progress()) {
		start_gc_or_evict(io->ctx, [ctx = io->ctx](int status) {
			process_pending_writes(ctx);
		});
	}

	// Start write directly - blocks will be buffered for 16KB alignment
	io->state = CacheIoState::HOST_WRITE_SUBMIT;

	// Record host write for QoS tracking
	cache->throttle_record_host_write(io->total_blocks);

	host_write_next_block(io);
}

//==============================================================================
// GC State Machine (batch-based: 1MB at a time for cache device)
//==============================================================================
static void gc_coalesced_read_done(void *cb_arg, int status);
static void gc_write_done(void *cb_arg, int status);
static void gc_start_batch(GcIo *io);
static void gc_submit_next_leftover(GcIo *io);
static void gc_start_writes(GcIo *io);
// Sequential read mode (128k chunks, pipeline read->write)
static void gc_start_seq_batch(GcIo *io);
static void gc_seq_read_done(void *cb_arg, int status);
static void gc_seq_write_done(void *cb_arg, int status);
static void gc_dispatch_writes(GcIo *io);
static void gc_dispatch_writes_msg(void *arg);

// Forward declaration for incremental GC
static void gc_start_reads(GcIo *io);

// GcWriteCtx for all GC write callbacks
struct GcWriteCtx {
	GcIo *io;
	struct iovec *iovs;
	bool is_leftover;  // true for leftover 4KB writes
};

// Callback for async finalize_gc completion (zone reset done or chunk done)
static void gc_finalize_done(void *cb_arg, int status)
{
	GcIo *io = static_cast<GcIo *>(cb_arg);

	if (status != 0) {
		io->last_status = status;
	}

	// Check if this was the final chunk (only when incremental mode is enabled)
	if (INCREMENTAL_GC_ENABLED && !io->prepare_result.is_final_chunk) {
		// More chunks to process - prepare next chunk
		bool prepared = io->ctx->cache->cache()->prepare_gc(io->prepare_result);
		if (!prepared) {
			// No more work or error - treat as done
			io->ctx->cache->set_gc_in_progress(false);
			gc_io_complete(io, io->last_status);
			return;
		}

		// Continue with next chunk
		io->completed_reads = 0;
		io->completed_writes = 0;
		io->total_gc_bytes += io->prepare_result.blocks_to_copy.size() * io->ctx->block_size;
		gc_start_reads(io);
		return;
	}

	// Final chunk done (or incremental disabled) - GC complete
	io->ctx->cache->set_gc_in_progress(false);
	gc_io_complete(io, io->last_status);
}

static void gc_start_reads(GcIo *io)
{
	io->last_status = 0;
	io->batch_start = 0;

	auto &blocks = io->prepare_result.blocks_to_copy;

	if (blocks.empty()) {
		// No valid blocks to copy, finalize asynchronously
		io->state = GcIoState::GC_SEGMENT_DONE;
		io->ctx->cache->cache()->finalize_gc_async(io->prepare_result, gc_finalize_done, io);
		return;
	}

	// Sequential read mode: build 128k chunk list
	if (io->use_sequential_read) {
		uint32_t block_size = io->ctx->block_size;
		io->seq_chunks.clear();
		io->seq_current_chunk = 0;

		// Group blocks by 128k chunk index
		std::map<size_t, std::vector<size_t>> chunk_map;
		for (size_t i = 0; i < blocks.size(); ++i) {
			// Calculate which 128k chunk this block belongs to (by src_idx in victim segment)
			size_t src_idx = blocks[i].src_idx;
			size_t chunk_idx = src_idx / GcIo::SEQ_CHUNK_BLOCKS;
			chunk_map[chunk_idx].push_back(i);
		}

		// Convert to seq_chunks vector
		for (auto &kv : chunk_map) {
			GcIo::SeqChunk sc;
			sc.chunk_idx = kv.first;
			sc.valid_block_indices = std::move(kv.second);
			io->seq_chunks.push_back(std::move(sc));
		}

		SPDK_NOTICELOG("GC seq mode: %zu blocks in %zu chunks\n",
			       blocks.size(), io->seq_chunks.size());

		gc_start_seq_batch(io);
		return;
	}

	// Scattered read mode: start first batch
	gc_start_batch(io);
}

// Start reading current batch (up to 1MB = 256 blocks)
static void gc_start_batch(GcIo *io)
{
	auto &blocks = io->prepare_result.blocks_to_copy;

	// // Record previous batch write time (if any)
	// if (io->batch_start > 0 && io->write_start > 0) {
	// 	io->write_ticks += spdk_get_ticks() - io->write_start;
	// }

	// Check if all batches done
	if (io->batch_start >= blocks.size()) {
		// All batches processed
		io->state = GcIoState::GC_SEGMENT_DONE;
		// CRITICAL: Do NOT finalize if writes failed - mapping would point to unwritten data
		if (io->last_status != 0) {
			SPDK_ERRLOG("GC: Skipping finalize due to write failure (status=%d), keeping source mappings\n", io->last_status);
			io->ctx->cache->cache()->abort_gc(io->prepare_result);
			io->ctx->cache->set_gc_in_progress(false);
			gc_io_complete(io, io->last_status);
			return;
		}
		io->ctx->cache->cache()->finalize_gc_async(io->prepare_result, gc_finalize_done, io);
		return;
	}

	io->state = GcIoState::READ_GC_SUBMIT;
	io->completed_reads = 0;
	io->completed_writes = 0;
	// io->read_start = spdk_get_ticks();  // Start read timing

	// Calculate batch size
	size_t remaining = blocks.size() - io->batch_start;
	io->batch_count = std::min(remaining, GcIo::BATCH_BLOCKS);

	uint32_t block_size = io->ctx->block_size;

	// Free previous staging buffer if any
	if (io->staging) {
		icache::dma_pool_free(io->staging, io->staging_size);
		io->staging = nullptr;
	}

	// Allocate DMA-capable staging buffer for this batch
	io->staging_size = io->batch_count * block_size;
	io->staging = icache::dma_pool_alloc(io->staging_size);
	if (!io->staging) {
		io->ctx->cache->set_gc_in_progress(false);
		gc_io_complete(io, -ENOMEM);
		return;
	}

	// Build coalesced reads (back merge consecutive blocks)
	io->coalesced_reads.clear();
	io->completed_coalesced_reads = 0;

	for (size_t i = 0; i < io->batch_count; ++i) {
		size_t block_idx = io->batch_start + i;
		auto &blk = blocks[block_idx];

		if (io->coalesced_reads.empty()) {
			// First block - start new range
			io->coalesced_reads.push_back({blk.src_offset, 1, i});
		} else {
			auto &last = io->coalesced_reads.back();
			uint64_t expected_offset = last.src_offset + last.num_blocks * block_size;
			if (blk.src_offset == expected_offset) {
				// Consecutive - merge
				last.num_blocks++;
			} else {
				// Not consecutive - start new range
				io->coalesced_reads.push_back({blk.src_offset, 1, i});
			}
		}
	}

	// Submit coalesced reads
	for (size_t r = 0; r < io->coalesced_reads.size(); ++r) {
		auto &cr = io->coalesced_reads[r];
		uint8_t *dest = static_cast<uint8_t*>(io->staging) + cr.first_idx * block_size;
		size_t read_len = cr.num_blocks * block_size;

		struct GcCoalescedReadCtx {
			GcIo *io;
			size_t coalesced_idx;
		};
		auto *read_ctx = new (std::nothrow) GcCoalescedReadCtx{io, r};
		if (!read_ctx) {
			io->last_status = -ENOMEM;
			io->completed_coalesced_reads++;
			continue;
		}

		int rc = io->ctx->device->read_cache_async(cr.src_offset, dest, read_len,
							   gc_coalesced_read_done, read_ctx);
		if (rc != 0) {
			SPDK_ERRLOG("GC: coalesced read failed for offset=0x%lx, len=%zu, rc=%d\n",
				    cr.src_offset, read_len, rc);
			delete read_ctx;
			io->last_status = rc;
			io->completed_coalesced_reads++;
		}
	}

	// Check if all reads completed synchronously (error case)
	if (io->completed_coalesced_reads >= io->coalesced_reads.size() && io->last_status != 0) {
		io->ctx->cache->set_gc_in_progress(false);
		gc_io_complete(io, io->last_status);
	}
}

static void gc_coalesced_read_done(void *cb_arg, int status)
{
	struct GcCoalescedReadCtx {
		GcIo *io;
		size_t coalesced_idx;
	};
	auto *ctx = static_cast<GcCoalescedReadCtx *>(cb_arg);
	GcIo *io = ctx->io;
	size_t coalesced_idx = ctx->coalesced_idx;
	delete ctx;

	if (status != 0) {
		SPDK_ERRLOG("GC: coalesced_read_done failed for idx=%zu, status=%d\n", coalesced_idx, status);
		if (io->last_status == 0) {
			io->last_status = status;
		}
	}

	io->completed_coalesced_reads++;

	// Check if all coalesced reads are done
	if (io->completed_coalesced_reads >= io->coalesced_reads.size()) {
		// // Record read time
		// io->read_ticks += spdk_get_ticks() - io->read_start;

		// All reads done, start writes for this batch
		if (io->last_status != 0) {
			io->ctx->cache->set_gc_in_progress(false);
			gc_io_complete(io, io->last_status);
			return;
		}
		gc_start_writes(io);
	}
}

// Start 64KB writes for current batch after all reads complete
static void gc_start_writes(GcIo *io)
{
	auto &blocks = io->prepare_result.blocks_to_copy;
	uint32_t block_size = io->ctx->block_size;

	// ZNS rule: 64KB aligned writes can be parallel within 1MB window
	//           Leftover (non-aligned) must be sequential at WP
	io->state = GcIoState::WRITE_GC_SUBMIT;
	// io->write_start = spdk_get_ticks();  // Start write timing

	// Calculate and store 64KB chunks and leftover counts
	io->num_64k_chunks = io->batch_count / GcIo::BLOCKS_PER_64K;
	io->leftover_blocks = io->batch_count % GcIo::BLOCKS_PER_64K;
	io->completed_64k_writes = 0;
	io->current_leftover_idx = 0;
	io->completed_writes = 0;

	// Pre-calculate expected writes (accounting for stripe boundary splits)
	// STRIPE_CHUNK_BLOCKS is defined as 32 in log_cache_segment.h
	io->expected_64k_writes = 0;
	for (size_t chunk = 0; chunk < io->num_64k_chunks; ++chunk) {
		size_t chunk_start = chunk * GcIo::BLOCKS_PER_64K;
		size_t first_block_idx = io->batch_start + chunk_start;
		size_t last_block_in_chunk = first_block_idx + GcIo::BLOCKS_PER_64K - 1;

		// Bounds check to prevent out-of-range access
		if (last_block_in_chunk >= blocks.size()) {
			io->expected_64k_writes += 1;  // Safe fallback
			continue;
		}

		size_t first_dst_idx = blocks[first_block_idx].dst_idx;
		size_t last_dst_idx = blocks[last_block_in_chunk].dst_idx;
		bool crosses_stripe = (first_dst_idx / STRIPE_CHUNK_BLOCKS) != (last_dst_idx / STRIPE_CHUNK_BLOCKS);
		io->expected_64k_writes += crosses_stripe ? GcIo::BLOCKS_PER_64K : 1;
	}

	// If no 64KB chunks, start leftover immediately (sequential)
	if (io->num_64k_chunks == 0) {
		if (io->leftover_blocks > 0) {
			gc_submit_next_leftover(io);
		} else {
			// No writes needed, move to next batch
			io->state = GcIoState::WRITE_GC_DONE;
			io->batch_start += io->batch_count;
			gc_start_batch(io);
		}
		return;
	}

	// Submit 64KB aligned chunks using scatter-gather (parallel, within 1MB window)
	// Check stripe boundary: STRIPE_CHUNK_BLOCKS=32, don't batch across stripe chunks
	for (size_t chunk = 0; chunk < io->num_64k_chunks; ++chunk) {
		size_t chunk_start = chunk * GcIo::BLOCKS_PER_64K;
		size_t first_block_idx = io->batch_start + chunk_start;
		size_t last_block_in_chunk = first_block_idx + GcIo::BLOCKS_PER_64K - 1;

		// Bounds check to prevent out-of-range access
		if (last_block_in_chunk >= blocks.size()) {
			io->completed_64k_writes++;  // Skip this chunk
			continue;
		}

		// Get dst_idx for first and last block in this 64KB chunk
		size_t first_dst_idx = blocks[first_block_idx].dst_idx;
		size_t last_dst_idx = blocks[last_block_in_chunk].dst_idx;

		// Check if they're in the same stripe chunk (STRIPE_CHUNK_BLOCKS = 32)
		bool crosses_stripe = (first_dst_idx / STRIPE_CHUNK_BLOCKS) != (last_dst_idx / STRIPE_CHUNK_BLOCKS);

		if (crosses_stripe) {
			// Stripe boundary crossed - write each block individually
			// GC uses placement handles 2~6 (host uses 0, 1)
			int gc_placement_handle = 2 + (io->prepare_result.gc_stream_id % (FDP_NUM_PLACEMENT_HANDLES - 2));
			for (size_t i = 0; i < GcIo::BLOCKS_PER_64K; ++i) {
				size_t block_idx = first_block_idx + i;
				uint8_t *src = static_cast<uint8_t*>(io->staging) + (chunk_start + i) * block_size;
				uint64_t dst_offset = blocks[block_idx].dst_offset;

				// Allocate single iovec for this block
				struct iovec *single_iov = static_cast<struct iovec*>(malloc(sizeof(struct iovec)));
				if (!single_iov) {
					io->last_status = -ENOMEM;
					io->completed_64k_writes++;
					continue;
				}
				single_iov->iov_base = src;
				single_iov->iov_len = block_size;

				auto *write_ctx = new (std::nothrow) GcWriteCtx{io, single_iov, false};
				if (!write_ctx) {
					free(single_iov);
					io->last_status = -ENOMEM;
					io->completed_64k_writes++;
					continue;
				}

				// Use writev with single iovec to pass placement handle
				int rc = io->ctx->device->writev_cache_async(dst_offset, single_iov, 1,
									     block_size, gc_write_done,
									     write_ctx, gc_placement_handle);
				if (rc == 0) {
					io->ctx->cache->add_gc_write_bytes(block_size);
					io->ctx->cache->add_cache_write_bytes(block_size);
				} else {
					free(single_iov);
					delete write_ctx;
					io->last_status = rc;
					io->completed_64k_writes++;
				}
			}
			continue;
		}

		// Allocate iovec for this 64KB chunk
		struct iovec *iovs = static_cast<struct iovec*>(calloc(GcIo::BLOCKS_PER_64K, sizeof(struct iovec)));
		if (!iovs) {
			io->last_status = -ENOMEM;
			io->completed_64k_writes++;
			continue;
		}

		// Setup iovecs
		uint64_t first_dst_offset = blocks[first_block_idx].dst_offset;
		for (size_t i = 0; i < GcIo::BLOCKS_PER_64K; ++i) {
			uint8_t *src = static_cast<uint8_t*>(io->staging) + (chunk_start + i) * block_size;
			iovs[i].iov_base = src;
			iovs[i].iov_len = block_size;
		}

		auto *write_ctx = new (std::nothrow) GcWriteCtx{io, iovs, false};
		if (!write_ctx) {
			free(iovs);
			io->last_status = -ENOMEM;
			io->completed_64k_writes++;
			continue;
		}

		// 64KB scatter-gather write with FDP placement handle
		// GC uses placement handles 2~6 (host uses 0, 1)
		int gc_placement_handle = 2 + (io->prepare_result.gc_stream_id % (FDP_NUM_PLACEMENT_HANDLES - 2));
		int rc = io->ctx->device->writev_cache_async(first_dst_offset, iovs, GcIo::BLOCKS_PER_64K,
							     GcIo::BLOCKS_PER_64K * block_size,
							     gc_write_done, write_ctx, gc_placement_handle);
		if (rc == 0) {
			// Track GC write bytes for WAF calculation
			io->ctx->cache->add_gc_write_bytes(GcIo::BLOCKS_PER_64K * block_size);
			io->ctx->cache->add_cache_write_bytes(GcIo::BLOCKS_PER_64K * block_size);
		} else {
			free(iovs);
			delete write_ctx;
			io->last_status = rc;
			io->completed_64k_writes++;
		}
	}

	// Check if all 64KB writes completed synchronously (error case)
	if (io->completed_64k_writes >= io->expected_64k_writes) {
		if (io->leftover_blocks > 0) {
			gc_submit_next_leftover(io);
		} else {
			io->state = GcIoState::WRITE_GC_DONE;
			io->batch_start += io->batch_count;
			gc_start_batch(io);
		}
	}
}

// Submit next leftover block sequentially (ZNS WP rule: non-aligned writes must be sequential)
static void gc_submit_next_leftover(GcIo *io)
{
	auto &blocks = io->prepare_result.blocks_to_copy;
	uint32_t block_size = io->ctx->block_size;

	// Check if all leftovers done
	if (io->current_leftover_idx >= io->leftover_blocks) {
		// All writes done, move to next batch
		io->state = GcIoState::WRITE_GC_DONE;
		io->batch_start += io->batch_count;
		gc_start_batch(io);
		return;
	}

	// Calculate leftover block position
	size_t leftover_start = io->num_64k_chunks * GcIo::BLOCKS_PER_64K;
	size_t block_idx = io->batch_start + leftover_start + io->current_leftover_idx;
	auto &blk = blocks[block_idx];
	uint8_t *src = static_cast<uint8_t*>(io->staging) +
	               (leftover_start + io->current_leftover_idx) * block_size;

	struct iovec *single_iov = static_cast<struct iovec*>(malloc(sizeof(struct iovec)));
	if (!single_iov) {
		io->last_status = -ENOMEM;
		io->current_leftover_idx++;
		gc_submit_next_leftover(io);
		return;
	}
	single_iov->iov_base = src;
	single_iov->iov_len = block_size;

	auto *write_ctx = new (std::nothrow) GcWriteCtx{io, single_iov, true};  // is_leftover = true
	if (!write_ctx) {
		free(single_iov);
		io->last_status = -ENOMEM;
		io->current_leftover_idx++;
		gc_submit_next_leftover(io);  // Try next
		return;
	}

	// 4KB sequential write with GC placement handle
	// GC uses placement handles 2~6 (host uses 0, 1)
	int gc_placement_handle = 2 + (io->prepare_result.gc_stream_id % (FDP_NUM_PLACEMENT_HANDLES - 2));
	int rc = io->ctx->device->writev_cache_async(blk.dst_offset, single_iov, 1, block_size,
	                                             gc_write_done, write_ctx, gc_placement_handle);
	if (rc == 0) {
		// Track GC write bytes for WAF calculation
		io->ctx->cache->add_gc_write_bytes(block_size);
		io->ctx->cache->add_cache_write_bytes(block_size);
	} else {
		free(single_iov);
		delete write_ctx;
		io->last_status = rc;
		io->current_leftover_idx++;
		gc_submit_next_leftover(io);  // Try next
	}
}

static void gc_write_done(void *cb_arg, int status)
{
	auto *ctx = static_cast<GcWriteCtx *>(cb_arg);
	GcIo *io = ctx->io;

	// Use explicit is_leftover flag instead of checking iovs
	bool is_64k_write = !ctx->is_leftover;

	// Free iovec array if it was a scatter-gather write
	if (ctx->iovs) {
		free(ctx->iovs);
	}
	delete ctx;

	if (status != 0) {
		SPDK_ERRLOG("GC: write_done failed, is_64k=%d, status=%d\n", is_64k_write, status);
		if (io->last_status == 0) {
			io->last_status = status;
		}
	}

	if (is_64k_write) {
		// 64KB-type write completed (normal batch or stripe-split individual)
		io->completed_64k_writes++;

		// Check if all expected writes done (including stripe-split individual writes)
		if (io->completed_64k_writes >= io->expected_64k_writes) {
			// Start leftover sequential writes (if any)
			if (io->leftover_blocks > 0) {
				gc_submit_next_leftover(io);
			} else {
				// No leftovers, move to next batch
				io->state = GcIoState::WRITE_GC_DONE;
				io->batch_start += io->batch_count;
				gc_start_batch(io);
			}
		}
	} else {
		// Leftover block completed - submit next one sequentially
		io->current_leftover_idx++;
		gc_submit_next_leftover(io);
	}
}

//==============================================================================
// GC Sequential Read Mode (128k chunks, pipeline read->write)
//==============================================================================

// Context for sequential read - includes chunk index
struct GcSeqReadCtx {
	GcIo *io;
	size_t batch_idx;  // Which chunk in current parallel batch
};

static void gc_start_seq_batch(GcIo *io)
{
	// // Record previous batch timing
	// if (io->seq_current_chunk > 0 && io->write_start > 0) {
	// 	io->write_ticks += spdk_get_ticks() - io->write_start;
	// }

	// Check if all chunks done
	if (io->seq_current_chunk >= io->seq_chunks.size()) {
		io->state = GcIoState::GC_SEGMENT_DONE;
		// CRITICAL: Do NOT finalize if writes failed - mapping would point to unwritten data
		if (io->last_status != 0) {
			SPDK_ERRLOG("GC-SEQ: Skipping finalize due to write failure (status=%d), keeping source mappings\n", io->last_status);
			io->ctx->cache->cache()->abort_gc(io->prepare_result);
			io->ctx->cache->set_gc_in_progress(false);
			gc_io_complete(io, io->last_status);
			return;
		}
		io->ctx->cache->cache()->finalize_gc_async(io->prepare_result, gc_finalize_done, io);
		return;
	}

	uint32_t block_size = io->ctx->block_size;
	constexpr size_t CHUNK_SIZE = GcIo::SEQ_CHUNK_BLOCKS * 4096;  // 128KB

	// Calculate batch size (up to SEQ_PARALLEL_CHUNKS = 8)
	size_t remaining = io->seq_chunks.size() - io->seq_current_chunk;
	io->seq_parallel_start = io->seq_current_chunk;
	io->seq_parallel_count = std::min(remaining, GcIo::SEQ_PARALLEL_CHUNKS);

	// Free previous staging buffer
	if (io->staging) {
		icache::dma_pool_free(io->staging, io->staging_size);
		io->staging = nullptr;
	}

	// Allocate staging for parallel chunks (8 * 128KB = 1MB)
	io->staging_size = io->seq_parallel_count * CHUNK_SIZE;
	io->staging = icache::dma_pool_alloc(io->staging_size);
	if (!io->staging) {
		io->ctx->cache->set_gc_in_progress(false);
		gc_io_complete(io, -ENOMEM);
		return;
	}

	// Count total writes expected
	io->seq_total_writes = 0;
	for (size_t i = 0; i < io->seq_parallel_count; ++i) {
		io->seq_total_writes += io->seq_chunks[io->seq_parallel_start + i].valid_block_indices.size();
	}

	// Debug: track batch progress
	static uint64_t gc_batch_count = 0;
	SPDK_DEBUGLOG(icache_gc, "GC-SEQ batch %lu: chunk %zu/%zu, parallel=%zu, writes_expected=%zu\n",
		      ++gc_batch_count, io->seq_current_chunk, io->seq_chunks.size(),
		      io->seq_parallel_count, io->seq_total_writes);

	io->seq_reads_done.store(0);
	io->seq_writes_done.store(0);
	io->pending_gc_writes.clear();
	io->pending_gc_write_idx = 0;
	// io->read_start = spdk_get_ticks();

	LogCacheSegment *victim = io->prepare_result.victim_seg;

	// Issue parallel 128k reads
	for (size_t batch_idx = 0; batch_idx < io->seq_parallel_count; ++batch_idx) {
		auto &sc = io->seq_chunks[io->seq_parallel_start + batch_idx];
		void *buf = static_cast<uint8_t*>(io->staging) + batch_idx * CHUNK_SIZE;

		// Calculate cache offset for this 128k chunk
		uint64_t cache_offset = victim->get_block_offset(sc.chunk_idx * GcIo::SEQ_CHUNK_BLOCKS, block_size);

		auto *read_ctx = new (std::nothrow) GcSeqReadCtx{io, batch_idx};
		if (!read_ctx) {
			io->last_status = -ENOMEM;
			io->seq_reads_done++;
			io->seq_writes_done.fetch_add(sc.valid_block_indices.size());
			continue;
		}

		int rc = io->ctx->device->read_cache_async(cache_offset, buf, CHUNK_SIZE,
							   gc_seq_read_done, read_ctx);
		if (rc != 0) {
			delete read_ctx;
			io->last_status = rc;
			io->seq_reads_done++;
			io->seq_writes_done.fetch_add(sc.valid_block_indices.size());
		}
	}

	// Check if all completed synchronously (only valid if no async reads in flight)
	// BUG FIX: Don't skip to next batch if reads are still in flight!
	// seq_reads_done tracks async reads; only proceed if all reads also done
	if (io->seq_writes_done.load() >= io->seq_total_writes &&
	    io->seq_reads_done.load() >= io->seq_parallel_count) {
		SPDK_NOTICELOG("GC-SEQ: batch sync complete (no valid blocks), moving to next batch\n");
		io->seq_current_chunk += io->seq_parallel_count;
		gc_start_seq_batch(io);
	}
}

static void gc_seq_read_done(void *cb_arg, int status)
{
	auto *ctx = static_cast<GcSeqReadCtx *>(cb_arg);
	GcIo *io = ctx->io;
	size_t batch_idx = ctx->batch_idx;
	delete ctx;

	if (status != 0) {
		io->last_status = status;
		// Mark all writes from this chunk as done (skip queueing)
		auto &sc = io->seq_chunks[io->seq_parallel_start + batch_idx];
		io->seq_writes_done.fetch_add(sc.valid_block_indices.size());
		size_t reads_done = ++io->seq_reads_done;
		if (reads_done == io->seq_parallel_count) {
			// io->read_ticks += spdk_get_ticks() - io->read_start;
			// io->write_start = spdk_get_ticks();
			io->pending_gc_write_idx = 0;
			// Dispatch any queued writes from successful reads
			if (!io->pending_gc_writes.empty()) {
				gc_dispatch_writes(io);
			} else if (io->seq_writes_done >= io->seq_total_writes) {
				// All writes skipped due to errors
				io->seq_current_chunk += io->seq_parallel_count;
				gc_start_seq_batch(io);
			}
		}
		return;
	}

	// Queue writes for this chunk (don't submit yet)
	auto &sc = io->seq_chunks[io->seq_parallel_start + batch_idx];
	auto &blocks = io->prepare_result.blocks_to_copy;
	uint32_t block_size = io->ctx->block_size;
	constexpr size_t CHUNK_SIZE = GcIo::SEQ_CHUNK_BLOCKS * 4096;

	for (size_t idx : sc.valid_block_indices) {
		auto &blk = blocks[idx];
		size_t block_in_chunk = blk.src_idx % GcIo::SEQ_CHUNK_BLOCKS;
		uint8_t *src = static_cast<uint8_t*>(io->staging) + batch_idx * CHUNK_SIZE + block_in_chunk * block_size;
		io->pending_gc_writes.push_back({blk.dst_offset, src, block_size});
	}

	size_t reads_done = ++io->seq_reads_done;
	if (reads_done == io->seq_parallel_count) {
		// All reads done - start dispatching writes with yield
		// io->read_ticks += spdk_get_ticks() - io->read_start;
		// io->write_start = spdk_get_ticks();
		io->pending_gc_write_idx = 0;

		// BUG FIX: If no valid blocks (seq_total_writes = 0), skip to next batch
		if (io->seq_total_writes == 0 || io->pending_gc_writes.empty()) {
			SPDK_NOTICELOG("GC-SEQ: no valid blocks in batch, skipping to next (reads=%zu, writes=%zu)\n",
				       reads_done, io->seq_total_writes);
			io->seq_current_chunk += io->seq_parallel_count;
			gc_start_seq_batch(io);
			return;
		}

		gc_dispatch_writes(io);
	}
}

static void gc_seq_write_done(void *cb_arg, int status)
{
	auto *ctx = static_cast<GcSeqReadCtx *>(cb_arg);
	GcIo *io = ctx->io;
	delete ctx;

	if (status != 0 && io->last_status == 0) {
		io->last_status = status;
	}

	size_t done = ++io->seq_writes_done;
	if (done >= io->seq_total_writes) {
		// io->write_ticks += spdk_get_ticks() - io->write_start;
		io->seq_current_chunk += io->seq_parallel_count;
		io->pending_gc_writes.clear();  // Clear queue for next batch
		gc_start_seq_batch(io);
	}
}

// Yield-based GC write dispatcher - submits up to GC_WRITES_PER_YIELD writes, then yields
// Uses writev_cache_async to go through global outstanding cmd limiting
static void gc_dispatch_writes(GcIo *io)
{
	auto &writes = io->pending_gc_writes;
	size_t submitted = 0;

	while (io->pending_gc_write_idx < writes.size() && submitted < GcIo::GC_WRITES_PER_YIELD) {
		auto &w = writes[io->pending_gc_write_idx++];

		// Allocate iovec for single block write
		struct iovec *iov = static_cast<struct iovec*>(malloc(sizeof(struct iovec)));
		if (!iov) {
			io->last_status = -ENOMEM;
			++io->seq_writes_done;
			continue;
		}
		iov->iov_base = w.src;
		iov->iov_len = w.block_size;

		auto *write_ctx = new (std::nothrow) GcSeqReadCtx{io, 0};
		if (!write_ctx) {
			free(iov);
			io->last_status = -ENOMEM;
			++io->seq_writes_done;
			continue;
		}

		// Use writev_cache_async to respect global outstanding cmd limit
		// GC uses placement handles 2~6 (host uses 0, 1)
		int gc_placement_handle = 2 + (io->prepare_result.gc_stream_id % (FDP_NUM_PLACEMENT_HANDLES - 2));
		int rc = io->ctx->device->writev_cache_async(w.dst_offset, iov, 1, w.block_size,
							     gc_seq_write_done, write_ctx, gc_placement_handle);
		if (rc == 0) {
			io->ctx->cache->add_gc_write_bytes(w.block_size);
			io->ctx->cache->add_cache_write_bytes(w.block_size);
			++submitted;
		} else {
			free(iov);
			delete write_ctx;
			io->last_status = rc;
			++io->seq_writes_done;
		}
	}

	// If more writes pending, yield and continue later
	if (io->pending_gc_write_idx < writes.size()) {
		spdk_thread_send_msg(spdk_get_thread(), gc_dispatch_writes_msg, io);
	}
}

static void gc_dispatch_writes_msg(void *arg)
{
	GcIo *io = static_cast<GcIo *>(arg);
	gc_dispatch_writes(io);
}

//==============================================================================
// Evict State Machine (128k chunk reads + pipelined 4k writes)
//==============================================================================

// Forward declaration for incremental evict
static void evict_start_chunk(EvictIo *io);

// Callback for async finalize_evict completion (zone reset done or chunk done)
static void evict_finalize_done(void *cb_arg, int status)
{
	EvictIo *io = static_cast<EvictIo *>(cb_arg);

	if (status != 0) {
		io->last_status = status;
	}

	// Check if this was the final chunk (only when incremental mode is enabled)
	if (INCREMENTAL_GC_ENABLED && !io->prepare_result.is_final_chunk) {
		// More chunks to process - prepare next chunk
		bool prepared = io->ctx->cache->cache()->prepare_evict(io->prepare_result);
		if (!prepared) {
			// No more work or error - treat as done
			io->ctx->cache->set_evict_in_progress(false);
			evict_io_complete(io, io->last_status);
			return;
		}

		// Accumulate total_bytes for accurate throughput calculation
		io->total_bytes += io->prepare_result.chunks.size() * 32 * io->ctx->block_size;

		// Continue with next chunk
		io->current_chunk_idx = 0;  // Reset chunk index for new batch
		evict_start_chunk(io);
		return;
	}

	// Final chunk done (or incremental disabled) - evict complete
	io->ctx->cache->set_evict_in_progress(false);
	evict_io_complete(io, io->last_status);
}

// Context for cache read - includes batch_idx for immediate write after read
struct CoalescedReadCtx {
	EvictIo *io;
	struct iovec *iovs;
	int iovcnt;
	size_t batch_idx;  // Which chunk in the batch this read is for
};

static void coalesced_read_done(void *cb_arg, int status);
static void evict_merge_writes(EvictIo *io);
static void evict_dispatch_writes(EvictIo *io);
static void evict_dispatch_writes_msg(void *arg);

static void evict_start_chunk(EvictIo *io)
{
	auto &chunks = io->prepare_result.chunks;

	if (io->current_chunk_idx >= chunks.size()) {
		// All chunks processed
		io->state = EvictIoState::EVICT_SEGMENT_DONE;
		// CRITICAL: Do NOT finalize if writes failed - would erase mapping for data not written to backend
		if (io->last_status != 0) {
			SPDK_ERRLOG("EVICT: Skipping finalize due to write failure (status=%d), keeping cache mappings\n", io->last_status);
			io->ctx->cache->cache()->abort_evict(io->prepare_result);
			io->ctx->cache->set_evict_in_progress(false);
			evict_io_complete(io, io->last_status);
			return;
		}
		io->ctx->cache->cache()->finalize_evict_async(io->prepare_result, evict_finalize_done, io);
		return;
	}

	io->state = EvictIoState::BACKEND_READ_BLOCK;
	// io->batch_read_start_ticks = spdk_get_ticks();
	io->batch_write_start_ticks = 0;
	uint32_t block_size = io->ctx->block_size;
	constexpr size_t CHUNK_BLOCKS = 32;  // 128k = 32 * 4k
	size_t chunk_size = CHUNK_BLOCKS * block_size;  // 128k

	// Calculate batch size (8 chunks = 1MB)
	size_t remaining = chunks.size() - io->current_chunk_idx;
	io->parallel_batch_start = io->current_chunk_idx;
	io->parallel_batch_count = std::min(remaining, EvictIo::PARALLEL_CHUNKS);

	// Free previous staging buffer
	if (io->staging) {
		icache::dma_pool_free(io->staging, io->staging_size);
		io->staging = nullptr;
	}

	// Allocate staging for 128k * batch_count
	io->staging_size = io->parallel_batch_count * chunk_size;
	io->staging = icache::dma_pool_alloc(io->staging_size);
	if (!io->staging) {
		io->ctx->cache->set_evict_in_progress(false);
		evict_io_complete(io, -ENOMEM);
		return;
	}

	// Count total valid blocks for write completion tracking
	size_t total_valid_blocks = 0;
	for (size_t i = 0; i < io->parallel_batch_count; ++i) {
		auto &chunk = chunks[io->parallel_batch_start + i];
		for (bool valid : chunk.valid_mask) {
			if (valid) total_valid_blocks++;
		}
	}

	io->parallel_reads_done.store(0);
	io->parallel_writes_done.store(0);
	io->coalesced_writes_total = total_valid_blocks;
	io->pending_evict_writes.clear();
	io->merged_evict_writes.clear();
	io->merged_evict_write_idx = 0;

	LogCacheSegment *victim = io->prepare_result.victim_seg;

	// Issue 8 x 128k reads in parallel
	for (size_t batch_idx = 0; batch_idx < io->parallel_batch_count; ++batch_idx) {
		size_t chunk_idx = io->parallel_batch_start + batch_idx;
		auto &chunk = chunks[chunk_idx];

		void *buf = static_cast<uint8_t*>(io->staging) + batch_idx * chunk_size;

		// Calculate cache offset: first block of this 128k chunk
		size_t first_seg_idx = chunk.cache_chunk_idx * CHUNK_BLOCKS;
		uint64_t cache_offset = victim->get_block_offset(first_seg_idx, block_size);

		auto *read_ctx = new (std::nothrow) CoalescedReadCtx{io, nullptr, 0, batch_idx};
		if (!read_ctx) {
			io->last_status = -ENOMEM;
			io->parallel_reads_done++;
			// Count all valid blocks in this chunk as done
			for (bool valid : chunk.valid_mask) {
				if (valid) io->parallel_writes_done++;
			}
			continue;
		}

		// Read 128k from cache
		int rc = io->ctx->device->read_cache_async(cache_offset, buf, chunk_size,
							   coalesced_read_done, read_ctx);
		if (rc != 0) {
			delete read_ctx;
			io->last_status = rc;
			io->parallel_reads_done++;
			for (bool valid : chunk.valid_mask) {
				if (valid) io->parallel_writes_done++;
			}
		}
	}

	// Check if all completed synchronously (error case)
	// BUG FIX: Must also check reads are done to avoid use-after-free on staging buffer
	if ((io->coalesced_writes_total == 0 ||
	     io->parallel_writes_done.load() >= io->coalesced_writes_total) &&
	    io->parallel_reads_done.load() >= io->parallel_batch_count) {
		SPDK_NOTICELOG("EVICT: batch sync complete, moving to next (writes=%zu/%zu, reads=%zu/%zu)\n",
			       io->parallel_writes_done.load(), io->coalesced_writes_total,
			       io->parallel_reads_done.load(), io->parallel_batch_count);
		io->current_chunk_idx += io->parallel_batch_count;
		evict_start_chunk(io);
	}
}

// Write completion callback for pipelined read->write
static void pipelined_write_done(void *cb_arg, int status);

static void coalesced_read_done(void *cb_arg, int status)
{
	auto *ctx = static_cast<CoalescedReadCtx *>(cb_arg);
	EvictIo *io = ctx->io;
	size_t batch_idx = ctx->batch_idx;

	delete[] ctx->iovs;
	delete ctx;

	auto &chunks = io->prepare_result.chunks;
	size_t chunk_idx = io->parallel_batch_start + batch_idx;
	auto &chunk = chunks[chunk_idx];
	uint32_t block_size = io->ctx->block_size;
	constexpr size_t CHUNK_BLOCKS = 32;
	size_t chunk_size = CHUNK_BLOCKS * block_size;

	// Count valid blocks in this chunk for error handling
	size_t valid_count = 0;
	for (bool valid : chunk.valid_mask) {
		if (valid) valid_count++;
	}

	if (status != 0) {
		if (io->last_status == 0) io->last_status = status;
		// Read failed - count all valid blocks as write done (skip queueing)
		io->parallel_writes_done.fetch_add(valid_count);
		size_t reads_done = ++io->parallel_reads_done;
		if (reads_done == io->parallel_batch_count) {
			// uint64_t read_elapsed = spdk_get_ticks() - io->batch_read_start_ticks;
			// io->read_total_us += read_elapsed * 1000000 / spdk_get_ticks_hz();
			// io->batch_write_start_ticks = spdk_get_ticks();
			// Dispatch any queued writes from successful reads
			if (!io->pending_evict_writes.empty()) {
				evict_merge_writes(io);
				io->merged_evict_write_idx = 0;
				io->coalesced_writes_total = io->merged_evict_writes.size();
				evict_dispatch_writes(io);
			} else if (io->parallel_writes_done >= io->coalesced_writes_total) {
				// All writes skipped due to errors
				io->current_chunk_idx += io->parallel_batch_count;
				evict_start_chunk(io);
			}
		}
		return;
	}

	// Queue writes for this chunk (don't submit yet)
	void *chunk_buf = static_cast<uint8_t*>(io->staging) + batch_idx * chunk_size;

	for (size_t i = 0; i < CHUNK_BLOCKS && i < chunk.valid_mask.size(); ++i) {
		if (!chunk.valid_mask[i]) continue;

		uint64_t backend_key = chunk.backend_keys[i];
		uint64_t backend_offset = backend_key * block_size;
		void *blk_buf = static_cast<uint8_t*>(chunk_buf) + i * block_size;
		io->pending_evict_writes.push_back({backend_offset, blk_buf, block_size});
	}

	size_t reads_done = ++io->parallel_reads_done;

	if (reads_done == io->parallel_batch_count) {
		// All reads done - start dispatching writes with yield
		// uint64_t read_elapsed = spdk_get_ticks() - io->batch_read_start_ticks;
		// io->read_total_us += read_elapsed * 1000000 / spdk_get_ticks_hz();
		// io->batch_write_start_ticks = spdk_get_ticks();

		// BUG FIX: If no valid blocks to write, skip to next batch
		if (io->coalesced_writes_total == 0 || io->pending_evict_writes.empty()) {
			SPDK_NOTICELOG("EVICT: no valid blocks in batch, skipping to next (reads=%zu, writes=%zu)\n",
				       reads_done, io->coalesced_writes_total);
			io->current_chunk_idx += io->parallel_batch_count;
			evict_start_chunk(io);
			return;
		}

		// Merge adjacent writes before dispatch
		evict_merge_writes(io);
		io->merged_evict_write_idx = 0;
		// Update coalesced_writes_total to merged count for completion tracking
		io->coalesced_writes_total = io->merged_evict_writes.size();

		evict_dispatch_writes(io);
	}
}

static void pipelined_write_done(void *cb_arg, int status)
{
	auto *ctx = static_cast<CoalescedReadCtx *>(cb_arg);
	EvictIo *io = ctx->io;
	delete ctx;

	if (status != 0 && io->last_status == 0) {
		io->last_status = status;
	}

	size_t done = ++io->parallel_writes_done;

	if (done >= io->coalesced_writes_total) {
		// // All writes done - record write timing
		// uint64_t write_start = io->batch_write_start_ticks ? io->batch_write_start_ticks : io->batch_read_start_ticks;
		// uint64_t elapsed = spdk_get_ticks() - write_start;
		// io->write_total_us += elapsed * 1000000 / spdk_get_ticks_hz();

		io->state = EvictIoState::BACKEND_WRITE_DONE;
		io->current_chunk_idx += io->parallel_batch_count;
		io->pending_evict_writes.clear();  // Clear queue for next batch
		io->merged_evict_writes.clear();
		evict_start_chunk(io);
	}
}

// Merge adjacent writes and prepare for dispatch
static void evict_merge_writes(EvictIo *io)
{
	auto &writes = io->pending_evict_writes;
	auto &merged = io->merged_evict_writes;
	merged.clear();

	if (writes.empty()) return;

	// Sort by backend_offset
	std::sort(writes.begin(), writes.end(),
		  [](const EvictIo::PendingEvictWrite &a, const EvictIo::PendingEvictWrite &b) {
			  return a.backend_offset < b.backend_offset;
		  });

	// Merge adjacent blocks
	merged.push_back({writes[0].backend_offset, {}, 0});
	merged.back().iovs.push_back({writes[0].src, writes[0].block_size});
	merged.back().total_len = writes[0].block_size;

	for (size_t i = 1; i < writes.size(); ++i) {
		auto &prev = merged.back();
		auto &cur = writes[i];

		// Check if adjacent (prev.offset + prev.total_len == cur.offset)
		if (prev.backend_offset + prev.total_len == cur.backend_offset) {
			// Merge into current group
			prev.iovs.push_back({cur.src, cur.block_size});
			prev.total_len += cur.block_size;
		} else {
			// Start new group
			merged.push_back({cur.backend_offset, {}, 0});
			merged.back().iovs.push_back({cur.src, cur.block_size});
			merged.back().total_len = cur.block_size;
		}
	}

	// Merge stats logging removed (too noisy)
}

// Yield-based Evict write dispatcher - submits up to EVICT_WRITES_PER_YIELD writes, then yields
static void evict_dispatch_writes(EvictIo *io)
{
	auto &merged = io->merged_evict_writes;
	size_t submitted = 0;

	while (io->merged_evict_write_idx < merged.size() && submitted < EvictIo::EVICT_WRITES_PER_YIELD) {
		auto &w = merged[io->merged_evict_write_idx++];

		auto *write_ctx = new (std::nothrow) CoalescedReadCtx{io, nullptr, 0, 0};
		if (!write_ctx) {
			io->last_status = -ENOMEM;
			++io->parallel_writes_done;
			continue;
		}

		int rc;
		if (w.iovs.size() == 1) {
			// Single block - use regular write
			rc = io->ctx->device->write_backend_async(w.backend_offset, w.iovs[0].iov_base,
								  w.total_len, pipelined_write_done, write_ctx);
		} else {
			// Multiple blocks - use writev
			rc = io->ctx->device->writev_backend_async(w.backend_offset, w.iovs.data(),
								   static_cast<int>(w.iovs.size()),
								   w.total_len, pipelined_write_done, write_ctx);
		}

		if (rc == 0) {
			// Track backend write bytes for stats
			io->ctx->cache->add_backend_write_bytes(w.total_len);
			++submitted;
		} else {
			delete write_ctx;
			io->last_status = rc;
			++io->parallel_writes_done;
		}
	}

	// If more writes pending, yield and continue later
	if (io->merged_evict_write_idx < merged.size()) {
		spdk_thread_send_msg(spdk_get_thread(), evict_dispatch_writes_msg, io);
	}
}

static void evict_dispatch_writes_msg(void *arg)
{
	EvictIo *io = static_cast<EvictIo *>(arg);
	evict_dispatch_writes(io);
}

// NOTE: Old evict_read_done, evict_start_parallel_writes, coalesced_write_done,
// and evict_write_done removed - now using pipelined model in coalesced_read_done
// and pipelined_write_done with yield-based dispatch

//==============================================================================
// GC/Evict Trigger
//==============================================================================

// Context for simple reset segment async callback
struct SimpleResetCtx {
	std::function<void(int)> on_complete;
	LogCacheAsync *cache;  // To clear evict_in_progress flag
};

static void simple_reset_done(void *cb_arg, int status)
{
	auto *ctx = static_cast<SimpleResetCtx *>(cb_arg);
	// Clear evict_in_progress flag since we set it before async reset
	if (ctx->cache) {
		ctx->cache->set_evict_in_progress(false);
	}
	if (ctx->on_complete) {
		ctx->on_complete(status);
	}
	delete ctx;
}

static void start_gc_or_evict(log_cache_ctx *ctx, std::function<void(int)> on_complete)
{
	LogCacheAsync *cache = ctx->cache.get();

	if (cache->gc_in_progress() || cache->evict_in_progress()) {
		return;
	}

	// Check if GC/evict is actually needed
	if (!cache->cache()->need_gc_or_evict()) {
		if (on_complete) on_complete(0);
		return;
	}


	// Try GC first (compaction)
	LogCache::GcPrepareResult gc_result;
	bool gc_prepared = cache->cache()->prepare_gc(gc_result);
	if (gc_prepared) {
		if (!gc_result.do_evict_only && !gc_result.blocks_to_copy.empty()) {
			// Do GC (compaction)
			cache->set_gc_in_progress(true);

			auto *gc_io = new (std::nothrow) GcIo();
			if (!gc_io) {
				cache->set_gc_in_progress(false);
				if (on_complete) on_complete(-ENOMEM);
				return;
			}

			gc_io->state = GcIoState::GC_SEGMENT_SUBMIT;
			gc_io->ctx = ctx;
			gc_io->prepare_result = std::move(gc_result);
			gc_io->completed_reads = 0;
			gc_io->completed_writes = 0;
			gc_io->last_status = 0;
			gc_io->on_complete = std::move(on_complete);
			// gc_io->start_ticks = spdk_get_ticks();
			gc_io->total_gc_bytes = gc_io->prepare_result.blocks_to_copy.size() * ctx->block_size;

			// Calculate valid ratio for read strategy
			auto *victim = gc_io->prepare_result.victim_seg;
			gc_io->segment_size_blocks = victim->blocks.size();
			gc_io->segment_base_offset = victim->physical_bases[0];
			gc_io->valid_ratio = (double)victim->valid_cnt / gc_io->segment_size_blocks;
			gc_io->use_sequential_read = (gc_io->valid_ratio >= 0.5);

			// Calculate score for debugging
			double u = (double)victim->valid_cnt / victim->blocks.size();
			if (u < 0.0001) u = 0.0001;
			double score = (g_threshold > 0 && g_timestamp > 0) ?
				std::min(g_threshold - (g_timestamp - victim->create_timestamp),
				         g_timestamp - victim->create_timestamp) * (1 - u) / u : 0.0;
			SPDK_NOTICELOG("GC: victim_seg=%p, valid_ratio=%.1f%%, blocks_to_copy=%zu, use_sequential_read=%d, g_threshold=%lu, g_timestamp=%lu, create_ts=%lu, age=%lu, score=%.2f\n",
				       (void*)victim, gc_io->valid_ratio * 100, gc_io->prepare_result.blocks_to_copy.size(),
				       gc_io->use_sequential_read,
				       g_threshold, g_timestamp, victim->create_timestamp,
				       g_timestamp - victim->create_timestamp, score);

			gc_io_state_machine(gc_io);
			return;
		}
	}

	// Fall back to evict
	LogCache::EvictPrepareResult evict_result;
	bool evict_prepared = cache->cache()->prepare_evict(evict_result);
	SPDK_NOTICELOG("Evict: prepare_evict returned %d, chunks=%zu, victim_seg=%p\n",
	       evict_prepared, evict_result.chunks.size(), evict_result.victim_seg);
	if (evict_prepared) {
		if (evict_result.chunks.empty() && evict_result.victim_seg) {
			// No valid blocks, just reset segment asynchronously
			SPDK_NOTICELOG("Evict: No valid blocks, resetting segment %p (stripe_width=%d)\n",
			       evict_result.victim_seg, evict_result.victim_seg->stripe_width_);
			for (int z = 0; z < evict_result.victim_seg->stripe_width_; z++) {
				SPDK_NOTICELOG("  Zone %d: physical_base=0x%lx\n", z, evict_result.victim_seg->physical_bases[z]);
			}
			auto *reset_ctx = new (std::nothrow) SimpleResetCtx();
			if (!reset_ctx) {
				if (on_complete) on_complete(-ENOMEM);
				return;
			}
			reset_ctx->on_complete = std::move(on_complete);
			reset_ctx->cache = cache;
			cache->set_evict_in_progress(true);
			cache->cache()->reset_segment_async(evict_result.victim_seg, simple_reset_done, reset_ctx);
			return;
		}

		cache->set_evict_in_progress(true);
		SPDK_NOTICELOG("Evict: Starting evict with %zu chunks, victim_seg=%p\n",
		       evict_result.chunks.size(), evict_result.victim_seg);

		auto *evict_io = new (std::nothrow) EvictIo();
		if (!evict_io) {
			cache->set_evict_in_progress(false);
			if (on_complete) on_complete(-ENOMEM);
			return;
		}

		evict_io->state = EvictIoState::EVICT_SEGMENT_SUBMIT;
		evict_io->ctx = ctx;
		// evict_io->start_ticks = spdk_get_ticks();
		evict_io->total_bytes = evict_result.chunks.size() * 32 * ctx->block_size;  // 128k per chunk

		// Calculate valid ratio for read strategy
		auto *victim = evict_result.victim_seg;
		evict_io->segment_size_blocks = victim->blocks.size();
		evict_io->segment_base_offset = victim->physical_bases[0];
		size_t valid_cnt = victim->valid_cnt;  // Actual valid block count
		evict_io->valid_ratio = (double)valid_cnt / evict_io->segment_size_blocks;
		evict_io->use_sequential_read = (evict_io->valid_ratio >= 0.5);

		SPDK_NOTICELOG("Evict: valid_ratio=%.1f%%, use_sequential_read=%d\n",
			       evict_io->valid_ratio * 100, evict_io->use_sequential_read);

		evict_io->prepare_result = std::move(evict_result);
		evict_io->current_chunk_idx = 0;
		evict_io->completed_reads = 0;
		evict_io->completed_writes = 0;
		evict_io->last_status = 0;
		evict_io->on_complete = std::move(on_complete);

		evict_io_state_machine(evict_io);
		return;
	}
	
	// Nothing to evict right now - evictor queue may be temporarily empty
	// This is normal when all segments have been evicted and new ones are filling up
	if (on_complete) on_complete(0);
}

static void process_pending_writes(log_cache_ctx *ctx)
{
	LogCacheAsync *cache = ctx->cache.get();

	// Start GC/Evict if needed (but don't block writes!)
	if (cache->need_gc_or_evict() && !cache->gc_in_progress() && !cache->evict_in_progress()) {
		start_gc_or_evict(ctx, [ctx](int status) {
			process_pending_writes(ctx);
		});
	}

	while (!cache->pending_writes().empty()) {
		// If still critical, can't make progress - wait for next GC completion
		if (cache->is_free_critical()) {
			return;
		}

		// Check QoS throttle (rate limit)
		if (cache->throttle_should_block()) {
			return;
		}

		CacheIo *io = cache->pending_writes().front();
		cache->pending_writes().pop_front();

		// Resume write
		io->state = CacheIoState::HOST_WRITE_SUBMIT;

		// If IO was blocked at start (not mid-write), record blocks now
		if (io->current_block_idx == 0) {
			cache->throttle_record_host_write(io->total_blocks);
		}

		host_write_next_block(io);
	}
}

//==============================================================================
// Main entry points
//==============================================================================
static void cache_io_state_machine(CacheIo *io)
{
	if (io->is_write) {
		cache_io_run_host_write(io);
	} else {
		cache_io_run_host_read(io);
	}
}

static void gc_io_state_machine(GcIo *io)
{
	gc_start_reads(io);
}

static void evict_io_state_machine(EvictIo *io)
{
	evict_start_chunk(io);
}

static bool
is_supported_cache_type(const char *type)
{
	if (type == nullptr || type[0] == '\0') {
		return true;
	}
	return strcasecmp(type, "LOG_GREEDY") == 0 ||
	       strcasecmp(type, "LOG_GREEDY_COST_BENEFIT_10") == 0 ||
	       strcasecmp(type, "LOG_GREEDY_COST_BENEFIT_11") == 0 ||
	       strcasecmp(type, "LOG_COST_BENEFIT") == 0;
}

//==============================================================================
// C API
//==============================================================================
extern "C" struct log_cache_ctx *
log_cache_ctx_create(struct spdk_bdev_desc *cache_desc,
		     struct spdk_bdev_desc *backend_desc,
		     uint64_t cache_block_count,
		     uint64_t backend_block_count,
		     uint32_t block_size,
		     const char *cache_type,
		     const char *waf_log_path,
		     const char *stat_log_path,
		     double valid_rate_threshold)
{
	if (!cache_desc || !backend_desc || cache_block_count == 0 || backend_block_count == 0 || block_size == 0) {
		return nullptr;
	}
	/*(cache_type)) {
		SPDK_ERRLOG("icache: unsupported cache type %s\n", cache_type);
		return nullptr;
	}*/

	auto ctx = std::make_unique<log_cache_ctx>();
	ctx->block_size = block_size;
	ctx->device = std::make_unique<icache::SpdkCacheDevice>(cache_desc, backend_desc, block_size);

	std::string waf_path = (waf_log_path && waf_log_path[0] != '\0') ? waf_log_path : "icache_waf.log";
	std::string stat_path = stat_log_path ? stat_log_path : "";
	uint64_t cold_capacity_bytes = backend_block_count * static_cast<uint64_t>(block_size);

	// ZNS: zone_size for physical_base alignment, zone_capacity for segment_bytes
	uint64_t zone_size_bytes = ctx->device->zone_size_bytes();
	uint64_t zone_capacity_bytes = ctx->device->zone_capacity_bytes();

	// Initialize DMA buffer pool for GC/Evict (2GB pre-allocated)
	if (!icache::DmaBufferPool::instance().init()) {
		SPDK_ERRLOG("icache: failed to initialize DMA buffer pool\n");
		return nullptr;
	}

	std::string cache_type_str = cache_type ? cache_type : "LOG_GREEDY";

	try {
		ctx->cache = std::make_unique<LogCacheAsync>(
			cold_capacity_bytes,
			cache_block_count,
			static_cast<int>(block_size),
			zone_size_bytes,
			zone_capacity_bytes,
			waf_path,
			stat_path,
			valid_rate_threshold,
			cache_type_str,
			ctx->device.get());
	} catch (const std::exception &ex) {
		SPDK_ERRLOG("icache: failed to construct LogCacheAsync: %s\n", ex.what());
		return nullptr;
	}

	// NOTE: poller는 worker thread에서 log_cache_ctx_move_poller_to_current_thread()로 등록
	ctx->write_buffer_poller = nullptr;

	return ctx.release();
}

extern "C" void
log_cache_ctx_destroy(struct log_cache_ctx *ctx)
{
	if (!ctx) {
		return;
	}
	// Unregister write buffer poller
	if (ctx->write_buffer_poller) {
		spdk_poller_unregister(&ctx->write_buffer_poller);
	}
	// Stop stats logger
	if (ctx->cache) {
		ctx->cache->stop_stats_logger();
	}
	// Flush remaining buffer before destroy
	if (ctx->cache && !ctx->cache->buffer_empty()) {
		ctx->cache->flush_write_buffer();
	}
	delete ctx;
}

// Register timeout poller on current thread (call from worker thread)
extern "C" void
log_cache_ctx_move_poller_to_current_thread(struct log_cache_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	// Start stats logger (always enabled)
	if (ctx->cache) {
		ctx->cache->start_stats_logger();
	}

	// Skip poller registration if write buffer is disabled
	if (!LogCacheAsync::WRITE_BUFFER_ENABLED) {
		SPDK_NOTICELOG("Write buffer disabled - skipping poller registration\n");
		return;
	}
	// Register poller on worker thread (poller는 create 시 등록 안함)
	ctx->write_buffer_poller = spdk_poller_register(write_buffer_timeout_poller, ctx,
							LogCacheAsync::WRITE_BUFFER_TIMEOUT_US);
	SPDK_NOTICELOG("Registered write_buffer poller on thread core %d\n",
		       spdk_env_get_current_core());
}

// Set io_channels for cache device (call once during worker init)
extern "C" void
log_cache_ctx_set_channels(struct log_cache_ctx *ctx,
			   struct spdk_io_channel *cache_ch,
			   struct spdk_io_channel *backend_ch)
{
	if (!ctx) {
		return;
	}
	ctx->device->set_channels(cache_ch, backend_ch);
	ctx->cache_ch = cache_ch;
	ctx->backend_ch = backend_ch;

	// Get NVMe controller from cache bdev for stats logger
	struct spdk_bdev *cache_bdev = ctx->device->get_cache_bdev();
	if (cache_bdev && ctx->cache) {
		struct spdk_nvme_ctrlr *nvme_ctrlr = bdev_nvme_get_ctrlr(cache_bdev);
		if (!nvme_ctrlr) {
			// Split partition case: try parent bdev (strip "pN" suffix)
			const char *bdev_name = spdk_bdev_get_name(cache_bdev);
			if (bdev_name) {
				std::string name(bdev_name);
				size_t p_pos = name.rfind('p');
				if (p_pos != std::string::npos && p_pos > 0) {
					std::string parent_name = name.substr(0, p_pos);
					struct spdk_bdev *parent_bdev = spdk_bdev_get_by_name(parent_name.c_str());
					if (parent_bdev) {
						nvme_ctrlr = bdev_nvme_get_ctrlr(parent_bdev);
						SPDK_NOTICELOG("Got NVMe controller from parent bdev %s\n", parent_name.c_str());
					}
				}
			}
		}
		if (nvme_ctrlr) {
			ctx->cache->set_stats_nvme_ctrlr(nvme_ctrlr);
			SPDK_NOTICELOG("Set NVMe controller for stats logger: %p\n", nvme_ctrlr);
		} else {
			SPDK_NOTICELOG("No NVMe controller for cache bdev (not an NVMe bdev?)\n");
		}
	}

	SPDK_NOTICELOG("Set io_channels: cache_ch=%p, backend_ch=%p\n", cache_ch, backend_ch);
}

extern "C" int
log_cache_ctx_write_async(struct log_cache_ctx *ctx,
			  struct spdk_io_channel *cache_ch,
			  struct spdk_io_channel *backend_ch,
			  uint64_t lba,
			  const struct iovec *iovs, int iovcnt, size_t total_len,
			  log_cache_io_done_cb cb_fn, void *cb_arg)
{
	(void)cache_ch;
	(void)backend_ch;


	if (!ctx || !iovs || iovcnt <= 0 || total_len == 0) {
		SPDK_ERRLOG("WRITE_ASYNC: invalid params ctx=%p iovs=%p iovcnt=%d len=%zu\n",
			    ctx, iovs, iovcnt, total_len);
		return -EINVAL;
	}

	auto io = new (std::nothrow) CacheIo();
	if (!io) {
		SPDK_ERRLOG("WRITE_ASYNC: failed to allocate CacheIo\n");
		return -ENOMEM;
	}
	io->state = CacheIoState::HOST_WRITE_SUBMIT;
	io->ctx = ctx;
	io->cb_fn = cb_fn;
	io->cb_arg = cb_arg;
	io->lba = lba;
	io->iovs = iovs;
	io->iovcnt = iovcnt;
	io->total_len = total_len;
	io->is_write = true;
	io->current_block_idx = 0;
	io->total_blocks = 0;
	io->completed_blocks = 0;
	io->last_status = 0;
	io->flushed_blocks.store(0);

	cache_io_state_machine(io);
	return 0;
}

extern "C" int
log_cache_ctx_read_async(struct log_cache_ctx *ctx,
			 struct spdk_io_channel *cache_ch,
			 struct spdk_io_channel *backend_ch,
			 uint64_t lba,
			 const struct iovec *iovs, int iovcnt, size_t total_len,
			 log_cache_io_done_cb cb_fn, void *cb_arg)
{
	(void)cache_ch;
	(void)backend_ch;


	if (!ctx || !iovs || iovcnt <= 0 || total_len == 0) {
		SPDK_ERRLOG("READ_ASYNC: invalid params ctx=%p iovs=%p iovcnt=%d len=%zu\n",
			    ctx, iovs, iovcnt, total_len);
		return -EINVAL;
	}

	auto io = new (std::nothrow) CacheIo();
	if (!io) {
		SPDK_ERRLOG("READ_ASYNC: failed to allocate CacheIo\n");
		return -ENOMEM;
	}
	io->state = CacheIoState::HOST_READ_SUBMIT;
	io->ctx = ctx;
	io->cb_fn = cb_fn;
	io->cb_arg = cb_arg;
	io->lba = lba;
	io->iovs = iovs;
	io->iovcnt = iovcnt;
	io->total_len = total_len;
	io->is_write = false;
	io->current_block_idx = 0;
	io->total_blocks = 0;
	io->completed_blocks = 0;
	io->last_status = 0;

	cache_io_state_machine(io);
	return 0;
}
