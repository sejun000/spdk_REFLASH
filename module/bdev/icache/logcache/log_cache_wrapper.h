#pragma once

#include <stdint.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_bdev_desc;
struct spdk_io_channel;

struct log_cache_ctx;

typedef void (*log_cache_io_done_cb)(void *cb_arg, int status);

struct log_cache_ctx *log_cache_ctx_create(struct spdk_bdev_desc *cache_desc,
					   struct spdk_bdev_desc *backend_desc,
					   uint64_t cache_block_count,
					   uint64_t backend_block_count,
					   uint32_t block_size,
					   const char *cache_type,
					   const char *waf_log_path,
					   const char *stat_log_path,
					   double valid_rate_threshold);

void log_cache_ctx_destroy(struct log_cache_ctx *ctx);

int log_cache_ctx_write_async(struct log_cache_ctx *ctx,
			      struct spdk_io_channel *cache_ch,
			      struct spdk_io_channel *backend_ch,
			      uint64_t lba,
			      const struct iovec *iovs, int iovcnt, size_t total_len,
			      log_cache_io_done_cb cb_fn, void *cb_arg);

int log_cache_ctx_read_async(struct log_cache_ctx *ctx,
			     struct spdk_io_channel *cache_ch,
			     struct spdk_io_channel *backend_ch,
			     uint64_t lba,
			     const struct iovec *iovs, int iovcnt, size_t total_len,
			     log_cache_io_done_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif
