#include "log_cache_wrapper.h"

#include <algorithm>
#include <atomic>
#include <cassert>
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
}

#include "port/cache_device.h"
#include "port/evict_policy_greedy.h"
#include "port/log_cache.h"
#include "port/log_cache_segment.h"

namespace icache {

//==============================================================================
// DMA Buffer Pool - Pre-allocated hugepage memory for GC/Evict operations
// Simple chunk-based design: 256KB chunks, LIFO stack (no sorting needed)
// - GC/Evict uses 256KB batches (64 x 4KB = 16 x 16KB aligned writes)
// - Remaining blocks < 256KB also use chunk (simple, slight waste is OK)
//==============================================================================
class DmaBufferPool {
public:
	static constexpr size_t CHUNK_SIZE = 256 * 1024;      // 256KB per chunk
	static constexpr size_t POOL_SIZE = 2ULL * 1024 * 1024 * 1024;  // 2GB total
	static constexpr size_t NUM_CHUNKS = POOL_SIZE / CHUNK_SIZE;    // 8192 chunks

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
};

// Forward declaration
class SpdkCacheDevice;

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
};

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

	bool can_submit(uint64_t offset, size_t len) const {
		// Can't submit if zone is not opened
		if (!zone_opened) {
			return false;
		}
		// Can't submit if flush is in progress (wait for device WP to advance)
		if (flush_in_progress) {
			return false;
		}
		// Can't write to already written area (ZNS sequential write constraint)
		if (offset < write_pointer) {
			return false;
		}
		if (is_aligned(offset, len)) {
			// Aligned: can submit if within ZRWA window from flushed_wp (device WP)
			return offset + len <= flushed_wp + ZONE_MAX_LBA_DISTANCE;
		} else {
			// Non-aligned: can only submit if it's exactly at write_pointer
			return offset == write_pointer;
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
		SPDK_ERRLOG("async_io_completion: IO failed!\n");
		assert(false && "Async IO failed");
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

	if (ctx->current_zone <= ctx->end_zone) {
		// More zones to reset
		zone_reset_next(ctx);
	} else {
		// All zones reset - clear ZoneQueue state
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
	int rc = spdk_bdev_zone_management(ctx->desc, ctx->ch, ctx->current_zone,
					   SPDK_BDEV_ZONE_RESET, zone_reset_completion, ctx);
	if (rc) {
		// Failed to submit, complete with error
		if (ctx->user_cb) {
			ctx->user_cb(ctx->user_cb_arg, rc);
		}
		delete ctx;
	}
}

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
			// Fallback: force ZNS mode with hardcoded zone size
			m_cache_zoned = true;
			m_cache_zone_blocks = 0x80000;  // 524288 blocks = 2GB zone size
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
	int writev_cache_async(uint64_t offset, struct iovec *iovs, int iovcnt, size_t total_len,
			       cache_device_io_cb cb, void *cb_arg)
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
			zq.pending.push(entry);
			return 0;
		}

		return submit_writev_direct(zone_id, offset, iovs, iovcnt, total_len, cb, cb_arg);
	}

	int submit_writev_direct(uint64_t zone_id, uint64_t offset, struct iovec *iovs, int iovcnt,
				 size_t total_len, cache_device_io_cb cb, void *cb_arg)
	{
		assert(iovs != nullptr && "submit_writev_direct: iovs is NULL");
		for (int i = 0; i < iovcnt; ++i) {
			assert(iovs[i].iov_base != nullptr && "submit_writev_direct: iov_base is NULL");
		}
		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(m_cache_bdev, offset, total_len, &block_offset, &num_blocks)) {
			return -EINVAL;
		}

		ZoneQueue &zq = zone_queues_[zone_id];
		zq.add_inflight(offset, total_len);

		auto *ctx = new (std::nothrow) ZoneAsyncIoCtx();
		if (!ctx) {
			zq.remove_inflight(offset);
			return -ENOMEM;
		}
		ctx->device = this;
		ctx->zone_id = zone_id;
		ctx->io_offset = offset;
		ctx->user_cb = cb;
		ctx->user_cb_arg = cb_arg;

		int rc = spdk_bdev_writev_blocks(m_cache_desc, m_cache_ch, iovs, iovcnt,
						 block_offset, num_blocks,
						 zone_async_io_completion, ctx);
		if (rc) {
			delete ctx;
			zq.remove_inflight(offset);
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

	int write_backend_async(uint64_t offset, const void *buf, size_t len,
				cache_device_io_cb cb, void *cb_arg) override
	{
		return submit_rw_async(m_backend_desc, m_backend_ch, m_backend_bdev,
				       buf, len, offset, true, cb, cb_arg);
	}

	int read_backend_async(uint64_t offset, void *buf, size_t len,
			       cache_device_io_cb cb, void *cb_arg) override
	{
		return submit_rw_async(m_backend_desc, m_backend_ch, m_backend_bdev,
				       buf, len, offset, false, cb, cb_arg);
	}

	int reset_cache_region_async(uint64_t offset, size_t len,
				     cache_device_io_cb cb, void *cb_arg) override
	{
		// Check if ZNS and valid parameters
		if (!m_cache_zoned || len == 0 || m_cache_zone_blocks == 0) {
			if (cb) {
				cb(cb_arg, 0);
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
		uint64_t block_offset, num_blocks;
		if (convert_to_blocks(bdev, byte_offset, len, &block_offset, &num_blocks)) {
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

		// Allocate context for zone-aware completion
		auto *zone_ctx = new (std::nothrow) ZoneAsyncIoCtx();
		if (!zone_ctx) {
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
							  submit_entry.len, submit_entry.cb, submit_entry.cb_arg);
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
			// Not a ZNS device, no zone open needed - mark zone as opened and process pending
			uint64_t zone_id = zone_slba / (m_cache_zone_blocks > 0 ? m_cache_zone_blocks : 1);
			ZoneQueue &zq = zone_queues_[zone_id];
			zq.zone_opened = true;
			zq.open_in_progress = false;
			// Initialize WP to zone start offset (byte unit)
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

	// Batch processing (256KB = 64 blocks at a time for cache device)
	// Matches DmaBufferPool::CHUNK_SIZE for efficient allocation
	static constexpr size_t BATCH_BLOCKS = 64;  // 256KB / 4KB
	size_t batch_start;      // Current batch start index in blocks_to_copy
	size_t batch_count;      // Number of blocks in current batch

	// 16KB chunks and leftover tracking (for ZNS write rules)
	size_t num_16k_chunks;       // Number of 16KB aligned chunks in current batch
	size_t leftover_blocks;      // Number of leftover blocks (0-3)
	size_t completed_16k_writes; // Completed 16KB chunk writes
	size_t current_leftover_idx; // Current leftover index being written (sequential)

	// Staging buffer for current batch (DMA-capable, hugepage)
	void *staging;
	size_t staging_size;

	// Completion callback for pending writes
	std::function<void(int)> on_complete;

	GcIo() : batch_start(0), batch_count(0), num_16k_chunks(0), leftover_blocks(0),
	         completed_16k_writes(0), current_leftover_idx(0), staging(nullptr), staging_size(0) {}
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

	// Batch processing (256KB = 64 blocks at a time)
	// Matches DmaBufferPool::CHUNK_SIZE for efficient allocation
	static constexpr size_t BATCH_BLOCKS = 64;  // 256KB / 4KB
	size_t batch_start;      // Current batch start within chunk
	size_t batch_count;      // Number of blocks in current batch

	// Staging buffer (DMA-capable, hugepage)
	void *staging;
	size_t staging_size;

	// Completion callback
	std::function<void(int)> on_complete;

	EvictIo() : batch_start(0), batch_count(0), staging(nullptr), staging_size(0) {}
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
		      icache::SpdkCacheDevice *device)
		: block_size_(block_size),
		  device_(device),
		  gc_in_progress_(false),
		  evict_in_progress_(false),
		  zone_size_bytes_(zone_size_bytes)
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

		// Hardcoded ZNS values for the test device
		static constexpr uint64_t ZNS_ZONE_SIZE_BLOCKS = 0x80000;      // 524288 blocks
		static constexpr uint64_t ZNS_ZONE_CAPACITY_BLOCKS = 0x43500;  // 275712 blocks
		const uint64_t zns_zone_size = ZNS_ZONE_SIZE_BLOCKS * block_size;
		const uint64_t zns_zone_capacity = ZNS_ZONE_CAPACITY_BLOCKS * block_size;

		// ZNS: segment_bytes = zone_capacity (writable space)
		// zone_size_bytes = for physical_base alignment
		Config cfg;
		cfg.segment_bytes = zns_zone_capacity;
		cfg.zone_size_bytes = zns_zone_size;

		cache_ = std::make_unique<LogCache>(
			cold_capacity,
			cache_block_count,
			block_size,
			false,
			std::string{},
			std::string{},
			waf_path,
			std::make_unique<GreedyEvictPolicy>(),
			&cfg,
			nullptr,
			valid_rate_threshold,
			nullptr,
			0.0,
			false,
			stat_log_path,
			device);
		cache_->set_stats_prefix("icache");
		cache_->set_async_mode(true);  // Disable sync eviction, use async wrapper
	}

	// Check if key exists in cache
	bool exists(long key) { return cache_->exists(key); }

	// Get block offset for a segment and index
	uint64_t block_offset(const LogCacheSegment *seg, size_t idx) const {
		return seg->physical_base + static_cast<uint64_t>(idx) * block_size_;
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

	// Pending writes waiting for GC/Evict
	std::list<CacheIo*>& pending_writes() { return pending_writes_; }

	// Check if free segments available
	bool need_gc_or_evict() { return cache_->is_cache_filled(); }

	// 16KB Write Buffer for aligned writes
	static constexpr size_t WRITE_BUFFER_SIZE = 16 * 1024;  // 16KB
	static constexpr uint64_t WRITE_BUFFER_TIMEOUT_US = 1000;  // 1ms

	struct BufferedBlock {
		CacheIo *io;
		uint32_t block_idx;
		uint64_t key;
		const uint8_t *buf;
	};

	// Add block to write buffer
	void buffer_add_block(CacheIo *io, uint32_t block_idx, uint64_t key, const uint8_t *buf) {
		write_buffer_.push_back({io, block_idx, key, buf});
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
		maybe_log_waf();
	}

	void add_gc_write_bytes(uint64_t bytes) {
		gc_write_bytes_ += bytes;
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
	uint64_t zone_size_bytes_;

	// 16KB Write Buffer
	std::vector<BufferedBlock> write_buffer_;
	bool buffer_timer_active_ = false;
	bool flush_pending_ = false;  // Set when flush needs retry after GC/Evict

	// WAF statistics
	FILE *waf_fp_ = nullptr;
	uint64_t host_write_bytes_ = 0;
	uint64_t gc_write_bytes_ = 0;
	uint64_t next_waf_log_threshold_ = WAF_LOG_INTERVAL;
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
};

static void write_buffer_flush_done(void *cb_arg, int status);
static void zone_write_done(void *cb_arg, int status);

// Zone write completion - called for each split write
static void zone_write_done(void *cb_arg, int status)
{
	auto *zctx = static_cast<ZoneWriteCtx*>(cb_arg);
	auto *flush_ctx = zctx->parent;


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
	std::vector<const uint8_t*> buf_ptrs;
	for (size_t i = 0; i < write_buffer_.size(); i++) {
		auto &blk = write_buffer_[i];
		uint64_t cache_offset;
		if (!cache_->append_block_metadata(0, static_cast<long>(blk.key),
						   static_cast<int>(block_size), &cache_offset)) {
			// No free segments - need GC/Evict
			delete flush_ctx;
			flush_pending_ = true;
			return;
		}
		flush_ctx->cache_offsets.push_back(cache_offset);
		buf_ptrs.push_back(blk.buf);
	}

	// Move buffer and count blocks per IO
	flush_ctx->blocks = std::move(write_buffer_);
	write_buffer_.clear();

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
	};
	std::vector<WriteGroup> groups;

	size_t i = 0;
	while (i < total_blocks) {
		WriteGroup group;
		group.start_idx = i;
		group.first_offset = flush_ctx->cache_offsets[i];
		group.count = 1;

		// Calculate zone boundary for this offset
		uint64_t zone_id = group.first_offset / zone_size;
		uint64_t zone_start = zone_id * zone_size;
		uint64_t zone_end = zone_start + zone_capacity;

		// Add consecutive blocks that stay within zone capacity
		while (i + group.count < total_blocks) {
			uint64_t expected_next = group.first_offset + group.count * block_size;
			uint64_t actual_next = flush_ctx->cache_offsets[i + group.count];

			// Check if consecutive
			if (actual_next != expected_next) {
				break;  // Not consecutive (probably zone transition)
			}

			// Check if next block would exceed zone capacity
			if (actual_next + block_size > zone_end) {
				break;  // Would exceed zone capacity
			}

			group.count++;
		}

		groups.push_back(group);
		i += group.count;
	}


	// Set outstanding writes count
	flush_ctx->outstanding_writes = static_cast<int>(groups.size());

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
		auto *zctx = new (std::nothrow) ZoneWriteCtx{flush_ctx, iovs, static_cast<int>(group.count)};
		if (!zctx) {
			free(iovs);
			flush_ctx->first_error = -ENOMEM;
			if (--flush_ctx->outstanding_writes == 0) {
				write_buffer_flush_done(flush_ctx, -ENOMEM);
			}
			continue;
		}

		size_t group_len = group.count * block_size;
		int rc = device_->writev_cache_async(group.first_offset, iovs, zctx->iovcnt,
						     group_len, zone_write_done, zctx);
		if (rc != 0) {
			SPDK_ERRLOG("writev_cache_async failed for group: %d\n", rc);
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

	// If flush is pending and we need GC/Evict, trigger it
	if (cache->flush_pending() && cache->need_gc_or_evict() &&
	    !cache->gc_in_progress() && !cache->evict_in_progress()) {
		start_gc_or_evict(ctx, [ctx](int status) {
			process_pending_writes(ctx);
		});
		return SPDK_POLLER_BUSY;
	}

	if (!cache->buffer_empty()) {
		cache->flush_write_buffer();
	}

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
	auto on_complete = std::move(io->on_complete);
	delete io;
	if (on_complete) {
		on_complete(status);
	}
}

static void evict_io_complete(EvictIo *io, int status)
{
	auto on_complete = std::move(io->on_complete);
	delete io;
	if (on_complete) {
		on_complete(status);
	}
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

	auto *read_ctx = new (std::nothrow) ReadBlockCtx{io, io->current_block_idx};
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
			SPDK_ERRLOG("READ_NEXT: failed to get cache location for key=%lu\n", key);
			delete read_ctx;
			cache_io_complete(io, -EIO);
			return;
		}
		io->state = CacheIoState::HOST_READ_CACHE_READ;
		int rc = cache->device()->read_cache_async(cache_offset, dest, block_size,
							   host_read_block_done, read_ctx);
		if (rc != 0) {
			SPDK_ERRLOG("READ_NEXT: read_cache_async failed rc=%d\n", rc);
			delete read_ctx;
			cache_io_complete(io, rc);
		}
	} else {
		// Read from backend
		uint64_t backend_offset = key * block_size;
		io->state = CacheIoState::HOST_READ_BACKEND_READ;
		int rc = cache->device()->read_backend_async(backend_offset, dest, block_size,
							     host_read_block_done, read_ctx);
		if (rc != 0) {
			SPDK_ERRLOG("READ_NEXT: read_backend_async failed rc=%d\n", rc);
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
	delete ctx;


	if (status != 0) {
		SPDK_ERRLOG("READ_DONE: error status=%d for blk=%lu\n", status, blk_idx);
		io->last_status = status;
	}
	io->current_block_idx++;
	host_read_next_block(io);
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
	io->total_blocks = io->total_len / io->ctx->block_size;
	io->current_block_idx = 0;
	io->completed_blocks = 0;
	io->last_status = 0;

	// No staging buffer needed - use iov directly (already hugepage-backed)
	host_read_next_block(io);
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
		// All blocks buffered for this IO
		// Don't complete here - completion will be called after flush_write_buffer completes
		return;
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

	// Add to 16KB write buffer instead of writing directly
	cache->buffer_add_block(io, io->current_block_idx, key, src);

	// Move to next block
	io->current_block_idx++;
	host_write_next_block(io);
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

	// Start write directly - blocks will be buffered for 16KB alignment
	io->state = CacheIoState::HOST_WRITE_SUBMIT;
	io->total_blocks = io->total_len / io->ctx->block_size;
	io->current_block_idx = 0;
	io->completed_blocks = 0;
	io->last_status = 0;
	host_write_next_block(io);
}

//==============================================================================
// GC State Machine (batch-based: 1MB at a time for cache device)
//==============================================================================
static void gc_read_done(void *cb_arg, int status);
static void gc_write_done(void *cb_arg, int status);
static void gc_start_batch(GcIo *io);
static void gc_submit_next_leftover(GcIo *io);

// Callback for async finalize_gc completion
static void gc_finalize_done(void *cb_arg, int status)
{
	GcIo *io = static_cast<GcIo *>(cb_arg);
	io->ctx->cache->set_gc_in_progress(false);
	gc_io_complete(io, status != 0 ? status : io->last_status);
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

	// Start first batch
	gc_start_batch(io);
}

// Start reading current batch (up to 1MB = 256 blocks)
static void gc_start_batch(GcIo *io)
{
	auto &blocks = io->prepare_result.blocks_to_copy;

	// Check if all batches done
	if (io->batch_start >= blocks.size()) {
		// All batches processed, finalize asynchronously
		io->state = GcIoState::GC_SEGMENT_DONE;
		io->ctx->cache->cache()->finalize_gc_async(io->prepare_result, gc_finalize_done, io);
		return;
	}

	io->state = GcIoState::READ_GC_SUBMIT;
	io->completed_reads = 0;
	io->completed_writes = 0;

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

	// Submit reads for this batch only
	for (size_t i = 0; i < io->batch_count; ++i) {
		size_t block_idx = io->batch_start + i;
		auto &blk = blocks[block_idx];
		uint8_t *dest = static_cast<uint8_t*>(io->staging) + i * block_size;

		struct GcReadCtx {
			GcIo *io;
			size_t idx;
		};
		auto *read_ctx = new (std::nothrow) GcReadCtx{io, i};
		if (!read_ctx) {
			io->last_status = -ENOMEM;
			io->completed_reads++;
			continue;
		}

		int rc = io->ctx->device->read_cache_async(blk.src_offset, dest, block_size,
							   gc_read_done, read_ctx);
		if (rc != 0) {
			delete read_ctx;
			io->last_status = rc;
			io->completed_reads++;
		}
	}

	// Check if all reads completed synchronously (error case)
	if (io->completed_reads >= io->batch_count && io->last_status != 0) {
		io->ctx->cache->set_gc_in_progress(false);
		gc_io_complete(io, io->last_status);
	}
}

static void gc_read_done(void *cb_arg, int status)
{
	struct GcReadCtx {
		GcIo *io;
		size_t idx;
	};
	auto *ctx = static_cast<GcReadCtx *>(cb_arg);
	GcIo *io = ctx->io;
	delete ctx;

	if (status != 0 && io->last_status == 0) {
		io->last_status = status;
	}

	io->completed_reads++;

	auto &blocks = io->prepare_result.blocks_to_copy;

	// Check if current batch reads are done
	if (io->completed_reads >= io->batch_count) {
		// Current batch reads done, start writes for this batch
		if (io->last_status != 0) {
			io->ctx->cache->set_gc_in_progress(false);
			gc_io_complete(io, io->last_status);
			return;
		}

		// Start writes for current batch
		// ZNS rule: 16KB aligned writes can be parallel within 1MB window
		//           Leftover (non-aligned) must be sequential at WP
		io->state = GcIoState::WRITE_GC_SUBMIT;

		uint32_t block_size = io->ctx->block_size;
		constexpr size_t BLOCKS_PER_16K = 4;  // 16KB / 4KB

		// Calculate and store 16KB chunks and leftover counts
		io->num_16k_chunks = io->batch_count / BLOCKS_PER_16K;
		io->leftover_blocks = io->batch_count % BLOCKS_PER_16K;
		io->completed_16k_writes = 0;
		io->current_leftover_idx = 0;
		io->completed_writes = 0;

		// If no 16KB chunks, start leftover immediately (sequential)
		if (io->num_16k_chunks == 0) {
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

		// Submit 16KB aligned chunks using scatter-gather (parallel, within 1MB window)
		// BATCH_BLOCKS=128 (512KB) ensures we stay within 1MB window
		for (size_t chunk = 0; chunk < io->num_16k_chunks; ++chunk) {
			size_t chunk_start = chunk * BLOCKS_PER_16K;

			// Allocate iovec for this chunk
			struct iovec *iovs = static_cast<struct iovec*>(calloc(BLOCKS_PER_16K, sizeof(struct iovec)));
			if (!iovs) {
				io->last_status = -ENOMEM;
				io->completed_16k_writes++;
				continue;
			}

			// Setup iovecs
			uint64_t first_dst_offset = blocks[io->batch_start + chunk_start].dst_offset;
			for (size_t i = 0; i < BLOCKS_PER_16K; ++i) {
				uint8_t *src = static_cast<uint8_t*>(io->staging) + (chunk_start + i) * block_size;
				iovs[i].iov_base = src;
				iovs[i].iov_len = block_size;
			}

			struct GcWriteCtx {
				GcIo *io;
				struct iovec *iovs;
			};
			auto *write_ctx = new (std::nothrow) GcWriteCtx{io, iovs};
			if (!write_ctx) {
				free(iovs);
				io->last_status = -ENOMEM;
				io->completed_16k_writes++;
				continue;
			}

			// 16KB scatter-gather write
			int rc = io->ctx->device->writev_cache_async(first_dst_offset, iovs, BLOCKS_PER_16K,
								     BLOCKS_PER_16K * block_size,
								     gc_write_done, write_ctx);
			if (rc == 0) {
				// Track GC write bytes for WAF calculation
				io->ctx->cache->add_gc_write_bytes(BLOCKS_PER_16K * block_size);
			} else {
				free(iovs);
				delete write_ctx;
				io->last_status = rc;
				io->completed_16k_writes++;
			}
		}

		// Check if all 16KB writes completed synchronously (error case)
		if (io->completed_16k_writes >= io->num_16k_chunks) {
			if (io->leftover_blocks > 0) {
				gc_submit_next_leftover(io);
			} else {
				io->state = GcIoState::WRITE_GC_DONE;
				io->batch_start += io->batch_count;
				gc_start_batch(io);
			}
		}
	}
}

// Submit next leftover block sequentially (ZNS WP rule: non-aligned writes must be sequential)
static void gc_submit_next_leftover(GcIo *io)
{
	auto &blocks = io->prepare_result.blocks_to_copy;
	uint32_t block_size = io->ctx->block_size;
	constexpr size_t BLOCKS_PER_16K = 4;

	// Check if all leftovers done
	if (io->current_leftover_idx >= io->leftover_blocks) {
		// All writes done, move to next batch
		io->state = GcIoState::WRITE_GC_DONE;
		io->batch_start += io->batch_count;
		gc_start_batch(io);
		return;
	}

	// Calculate leftover block position
	size_t leftover_start = io->num_16k_chunks * BLOCKS_PER_16K;
	size_t block_idx = io->batch_start + leftover_start + io->current_leftover_idx;
	auto &blk = blocks[block_idx];
	uint8_t *src = static_cast<uint8_t*>(io->staging) +
	               (leftover_start + io->current_leftover_idx) * block_size;

	struct GcWriteCtx {
		GcIo *io;
		struct iovec *iovs;
	};
	auto *write_ctx = new (std::nothrow) GcWriteCtx{io, nullptr};
	if (!write_ctx) {
		io->last_status = -ENOMEM;
		io->current_leftover_idx++;
		gc_submit_next_leftover(io);  // Try next
		return;
	}

	// 4KB sequential write at WP
	int rc = io->ctx->device->write_cache_async(blk.dst_offset, src, block_size,
	                                            gc_write_done, write_ctx);
	if (rc == 0) {
		// Track GC write bytes for WAF calculation
		io->ctx->cache->add_gc_write_bytes(block_size);
	} else {
		delete write_ctx;
		io->last_status = rc;
		io->current_leftover_idx++;
		gc_submit_next_leftover(io);  // Try next
	}
}

static void gc_write_done(void *cb_arg, int status)
{
	struct GcWriteCtx {
		GcIo *io;
		struct iovec *iovs;
	};
	auto *ctx = static_cast<GcWriteCtx *>(cb_arg);
	GcIo *io = ctx->io;

	bool is_16k_write = (ctx->iovs != nullptr);

	// Free iovec array if it was a scatter-gather write
	if (ctx->iovs) {
		free(ctx->iovs);
	}
	delete ctx;

	if (status != 0 && io->last_status == 0) {
		io->last_status = status;
	}

	if (is_16k_write) {
		// 16KB chunk completed
		io->completed_16k_writes++;

		// Check if all 16KB chunks done
		if (io->completed_16k_writes >= io->num_16k_chunks) {
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
// Evict State Machine
//==============================================================================
static void evict_read_done(void *cb_arg, int status);
static void evict_write_done(void *cb_arg, int status);

// Callback for async finalize_evict completion
static void evict_finalize_done(void *cb_arg, int status)
{
	EvictIo *io = static_cast<EvictIo *>(cb_arg);
	io->ctx->cache->set_evict_in_progress(false);
	evict_io_complete(io, status != 0 ? status : io->last_status);
}

static void evict_start_chunk(EvictIo *io)
{
	auto &chunks = io->prepare_result.chunks;

	if (io->current_chunk_idx >= chunks.size()) {
		// All chunks processed - finalize evict asynchronously
		io->state = EvictIoState::EVICT_SEGMENT_DONE;
		io->ctx->cache->cache()->finalize_evict_async(io->prepare_result, evict_finalize_done, io);
		return;
	}

	io->state = EvictIoState::BACKEND_READ_BLOCK;
	auto &chunk = chunks[io->current_chunk_idx];
	uint32_t block_size = io->ctx->block_size;
	size_t chunk_blocks = chunk.valid_mask.size();

	// Free previous staging buffer if any
	if (io->staging) {
		icache::dma_pool_free(io->staging, io->staging_size);
		io->staging = nullptr;
	}

	// Allocate DMA-capable staging for this chunk
	io->staging_size = chunk_blocks * block_size;
	io->staging = icache::dma_pool_alloc(io->staging_size);
	if (!io->staging) {
		io->ctx->cache->set_evict_in_progress(false);
		evict_io_complete(io, -ENOMEM);
		return;
	}

	io->completed_reads = 0;

	// Read all blocks in chunk (from cache if valid, from backend if not)
	for (size_t i = 0; i < chunk_blocks; ++i) {
		uint8_t *dest = static_cast<uint8_t*>(io->staging) + i * block_size;
		uint64_t key = chunk.start_key + i;

		struct EvictReadCtx {
			EvictIo *io;
			size_t block_in_chunk;
		};
		auto *read_ctx = new (std::nothrow) EvictReadCtx{io, i};
		if (!read_ctx) {
			io->last_status = -ENOMEM;
			io->completed_reads++;
			continue;
		}

		int rc;
		if (chunk.valid_mask[i]) {
			// Read from cache - get cache offset
			uint64_t cache_offset;
			if (io->ctx->cache->cache()->get_cache_location(static_cast<long>(key), &cache_offset)) {
				rc = io->ctx->device->read_cache_async(cache_offset, dest, block_size,
								       evict_read_done, read_ctx);
				if (rc != 0) {
					delete read_ctx;
					io->last_status = rc;
					io->completed_reads++;
				}
			} else {
				// Cache miss (shouldn't happen), read from backend
				uint64_t backend_offset = key * block_size;
				rc = io->ctx->device->read_backend_async(backend_offset, dest, block_size,
									 evict_read_done, read_ctx);
				if (rc != 0) {
					delete read_ctx;
					io->last_status = rc;
					io->completed_reads++;
				}
			}
		} else {
			// Read from backend
			uint64_t backend_offset = key * block_size;
			rc = io->ctx->device->read_backend_async(backend_offset, dest, block_size,
								 evict_read_done, read_ctx);
			if (rc != 0) {
				delete read_ctx;
				io->last_status = rc;
				io->completed_reads++;
			}
		}
	}

	// Check if all reads completed synchronously (error case)
	if (io->completed_reads >= chunk_blocks) {
		// All reads done, start write
		io->state = EvictIoState::BACKEND_WRITE_BLOCK;

		uint64_t backend_offset = chunk.start_key * block_size;

		struct EvictWriteCtx {
			EvictIo *io;
			size_t chunk_idx;
		};
		auto *write_ctx = new (std::nothrow) EvictWriteCtx{io, io->current_chunk_idx};
		if (!write_ctx) {
			io->last_status = -ENOMEM;
			io->current_chunk_idx++;
			evict_start_chunk(io);
			return;
		}

		int rc = io->ctx->device->write_backend_async(backend_offset, io->staging,
							      chunk_blocks * block_size,
							      evict_write_done, write_ctx);
		if (rc != 0) {
			delete write_ctx;
			io->last_status = rc;
			io->current_chunk_idx++;
			evict_start_chunk(io);
		}
	}
}

static void evict_read_done(void *cb_arg, int status)
{
	struct EvictReadCtx {
		EvictIo *io;
		size_t block_in_chunk;
	};
	auto *ctx = static_cast<EvictReadCtx *>(cb_arg);
	EvictIo *io = ctx->io;
	delete ctx;

	if (status != 0 && io->last_status == 0) {
		io->last_status = status;
	}

	io->completed_reads++;

	auto &chunk = io->prepare_result.chunks[io->current_chunk_idx];
	size_t chunk_blocks = chunk.valid_mask.size();

	if (io->completed_reads >= chunk_blocks) {
		// All reads done, start write
		io->state = EvictIoState::BACKEND_WRITE_BLOCK;
		uint32_t block_size = io->ctx->block_size;
		uint64_t backend_offset = chunk.start_key * block_size;

		struct EvictWriteCtx {
			EvictIo *io;
			size_t chunk_idx;
		};
		auto *write_ctx = new (std::nothrow) EvictWriteCtx{io, io->current_chunk_idx};
		if (!write_ctx) {
			io->last_status = -ENOMEM;
			io->current_chunk_idx++;
			evict_start_chunk(io);
			return;
		}

		int rc = io->ctx->device->write_backend_async(backend_offset, io->staging,
							      chunk_blocks * block_size,
							      evict_write_done, write_ctx);
		if (rc != 0) {
			delete write_ctx;
			io->last_status = rc;
			io->current_chunk_idx++;
			evict_start_chunk(io);
		}
	}
}

static void evict_write_done(void *cb_arg, int status)
{
	struct EvictWriteCtx {
		EvictIo *io;
		size_t chunk_idx;
	};
	auto *ctx = static_cast<EvictWriteCtx *>(cb_arg);
	EvictIo *io = ctx->io;
	delete ctx;

	if (status != 0 && io->last_status == 0) {
		io->last_status = status;
	}

	io->state = EvictIoState::BACKEND_WRITE_DONE;
	io->current_chunk_idx++;
	evict_start_chunk(io);
}

//==============================================================================
// GC/Evict Trigger
//==============================================================================

// Context for simple reset segment async callback
struct SimpleResetCtx {
	std::function<void(int)> on_complete;
};

static void simple_reset_done(void *cb_arg, int status)
{
	auto *ctx = static_cast<SimpleResetCtx *>(cb_arg);
	if (ctx->on_complete) {
		ctx->on_complete(status);
	}
	delete ctx;
}

static void start_gc_or_evict(log_cache_ctx *ctx, std::function<void(int)> on_complete)
{
	LogCacheAsync *cache = ctx->cache.get();

	if (cache->gc_in_progress() || cache->evict_in_progress()) {
		// Already running
		if (on_complete) on_complete(0);
		return;
	}

	if (!cache->cache()->need_gc_or_evict()) {
		// No need to GC/Evict
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

			gc_io_state_machine(gc_io);
			return;
		}
	}

	// Fall back to evict
	LogCache::EvictPrepareResult evict_result;
	bool evict_prepared = cache->cache()->prepare_evict(evict_result);
	if (evict_prepared) {
		if (evict_result.chunks.empty() && evict_result.victim_seg) {
			// No valid blocks, just reset segment asynchronously
			auto *reset_ctx = new (std::nothrow) SimpleResetCtx();
			if (!reset_ctx) {
				if (on_complete) on_complete(-ENOMEM);
				return;
			}
			reset_ctx->on_complete = std::move(on_complete);
			cache->cache()->reset_segment_async(evict_result.victim_seg, simple_reset_done, reset_ctx);
			return;
		}

		cache->set_evict_in_progress(true);

		auto *evict_io = new (std::nothrow) EvictIo();
		if (!evict_io) {
			cache->set_evict_in_progress(false);
			if (on_complete) on_complete(-ENOMEM);
			return;
		}

		evict_io->state = EvictIoState::EVICT_SEGMENT_SUBMIT;
		evict_io->ctx = ctx;
		evict_io->prepare_result = std::move(evict_result);
		evict_io->current_chunk_idx = 0;
		evict_io->completed_reads = 0;
		evict_io->completed_writes = 0;
		evict_io->last_status = 0;
		evict_io->on_complete = std::move(on_complete);

		evict_io_state_machine(evict_io);
		return;
	}

	// Nothing to do
	SPDK_ERRLOG("start_gc_or_evict: both prepare_gc and prepare_evict failed!\n");
	if (on_complete) on_complete(0);
}

static void process_pending_writes(log_cache_ctx *ctx)
{
	LogCacheAsync *cache = ctx->cache.get();

	// Retry pending flush_write_buffer if needed
	if (cache->flush_pending() && !cache->need_gc_or_evict()) {
		cache->set_flush_pending(false);
		cache->flush_write_buffer();
	}

	while (!cache->pending_writes().empty()) {
		if (cache->need_gc_or_evict()) {
			// Need more GC/Evict
			if (!cache->gc_in_progress() && !cache->evict_in_progress()) {
				start_gc_or_evict(ctx, [ctx](int status) {
					process_pending_writes(ctx);
				});
			}
			return;
		}

		CacheIo *io = cache->pending_writes().front();
		cache->pending_writes().pop_front();

		// Resume write
		io->state = CacheIoState::HOST_WRITE_SUBMIT;
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
	return strcasecmp(type, "LOG_GREEDY") == 0;
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
	if (!is_supported_cache_type(cache_type)) {
		SPDK_ERRLOG("icache: unsupported cache type %s\n", cache_type);
		return nullptr;
	}

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
