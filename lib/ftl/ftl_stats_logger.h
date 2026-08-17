/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#ifndef FTL_STATS_LOGGER_H
#define FTL_STATS_LOGGER_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NVMe FDP Statistics Log Page (Log ID 0x20)
 * Used to read HBMW (Host Bytes with Metadata Written) and MBMW (Media Bytes with Metadata Written)
 * for WAF calculation on FDP-enabled SSDs.
 *
 * Read with: nvme fdp stats /dev/nvmeX -e <endurance_group_id>
 */
#pragma pack(push, 1)
struct ftl_nvme_fdp_stats_log {
	uint8_t hbmw[16];    /* Host Bytes with Metadata Written (128-bit, in bytes) */
	uint8_t mbmw[16];    /* Media Bytes with Metadata Written (128-bit, in bytes) */
	uint8_t mbe[16];     /* Media Bytes Erased (128-bit, in bytes) */
	uint8_t rsvd48[16];  /* Reserved */
};  /* Total: 64 bytes (matches nvme-cli/libnvme) */
#pragma pack(pop)

struct ftl_backend_vendor_log {
	uint8_t data[256];
};

/**
 * FTL Stats Logger
 *
 * Periodically logs write statistics to a CSV file:
 * - Host Write: bytes received from host
 * - Cache Write: bytes written to cache device (nv_cache)
 * - Backend Write: bytes written to backend device (base_bdev)
 * - Host Written: NVMe data_units_written from endurance log
 * - Media Written: NVMe media_units_written from endurance log
 */
struct ftl_stats_logger {
	/* Device name for log file naming */
	char *dev_name;

	/* Log directory and file path */
	char log_dir[256];
	char log_path[512];

	/* Logging interval in microseconds */
	uint64_t interval_us;

	/* Log file handle */
	FILE *log_fp;

	/* SPDK poller for periodic logging */
	struct spdk_poller *poller;

	/* Started flag */
	bool started;

	/* Stats counters */
	uint64_t host_write_bytes;
	uint64_t cache_write_bytes;
	uint64_t backend_write_bytes;  /* Compaction + host-level GC data writes */
	uint64_t compaction_write_bytes;
	uint64_t gc_write_bytes;
	uint64_t backend_md_write_bytes;
	uint64_t valid_blocks;         /* Valid 4K blocks in nv_cache */
	uint64_t write_hit_count;      /* Write cache hits (L2P overwrite) */
	uint64_t gc_victim_blocks;     /* Blocks compacted by GC */
	uint64_t evict_victim_blocks;  /* Blocks evicted (chunk freed) */

	/* Per-metadata-type write bytes to nv_cache */
	uint64_t l2p_write_bytes;      /* L2P cache page writeback (eviction + persist) */
	uint64_t md_write_bytes;       /* ftl_md region writes to nv_cache */

	/* Previous values for delta calculation */
	uint64_t prev_host_write;
	uint64_t prev_cache_write;
	uint64_t prev_backend_write;
	uint64_t prev_compaction_write;
	uint64_t prev_gc_write;
	uint64_t prev_backend_md_write;
	uint64_t prev_l2p_write;
	uint64_t prev_md_write;

	/* Start time */
	uint64_t start_time_us;

	/* NVMe log page reading */
	struct spdk_bdev_desc *cache_bdev_desc;
	struct spdk_io_channel *cache_ioch;
	struct spdk_nvme_ctrlr *nvme_ctrlr;
	struct ftl_nvme_fdp_stats_log *log_page_buf;
	bool log_page_pending;

	/* NVMe FDP stats (from log page, in bytes) */
	uint64_t nvme_host_written;   /* HBMW: Host Bytes with Metadata Written */
	uint64_t nvme_media_written;  /* MBMW: Media Bytes with Metadata Written */
	uint64_t prev_nvme_host_written;
	uint64_t prev_nvme_media_written;

	/* Backend QLC physical NAND writes (vendor log page 0xC0). */
	struct spdk_nvme_ctrlr *backend_nvme_ctrlr;
	struct ftl_backend_vendor_log *backend_log_page_buf;
	bool backend_log_page_pending;
	uint64_t backend_nand_written;
	uint64_t prev_backend_nand_written;
};

/**
 * Create and initialize a stats logger
 *
 * @param dev_name Device name for log file naming
 * @param log_dir Directory for log files (default: "logging")
 * @param interval_us Logging interval in microseconds (default: 2000000 = 2 sec)
 * @return Pointer to stats logger, or NULL on failure
 */
struct ftl_stats_logger *ftl_stats_logger_create(const char *dev_name,
						  const char *log_dir,
						  uint64_t interval_us);

/**
 * Destroy a stats logger
 *
 * @param logger Stats logger to destroy
 */
void ftl_stats_logger_destroy(struct ftl_stats_logger *logger);

/**
 * Set cache bdev for NVMe log page reading
 *
 * @param logger Stats logger
 * @param bdev_desc Cache bdev descriptor
 * @param ioch IO channel for the cache bdev
 */
void ftl_stats_logger_set_cache_bdev(struct ftl_stats_logger *logger,
				      struct spdk_bdev_desc *bdev_desc,
				      struct spdk_io_channel *ioch);

/**
 * Set NVMe controller for log page reading
 *
 * @param logger Stats logger
 * @param ctrlr NVMe controller
 */
void ftl_stats_logger_set_nvme_ctrlr(struct ftl_stats_logger *logger,
				      struct spdk_nvme_ctrlr *ctrlr);

void ftl_stats_logger_set_backend_nvme_ctrlr(struct ftl_stats_logger *logger,
					      struct spdk_nvme_ctrlr *ctrlr);

/**
 * Start the stats logger (call from SPDK thread)
 *
 * @param logger Stats logger
 * @return 0 on success, negative errno on failure
 */
int ftl_stats_logger_start(struct ftl_stats_logger *logger);

/**
 * Stop the stats logger
 *
 * @param logger Stats logger
 */
void ftl_stats_logger_stop(struct ftl_stats_logger *logger);

/**
 * Add host write bytes
 */
static inline void
ftl_stats_logger_add_host_write(struct ftl_stats_logger *logger, uint64_t bytes)
{
	if (logger) {
		logger->host_write_bytes += bytes;
	}
}

/**
 * Add cache write bytes
 */
static inline void
ftl_stats_logger_add_cache_write(struct ftl_stats_logger *logger, uint64_t bytes)
{
	if (logger) {
		logger->cache_write_bytes += bytes;
	}
}

/**
 * Add backend write bytes
 */
static inline void
ftl_stats_logger_add_backend_write(struct ftl_stats_logger *logger, uint64_t bytes)
{
	if (logger) {
		logger->backend_write_bytes += bytes;
	}
}

/** Add completed compaction data writes to the backend. */
static inline void
ftl_stats_logger_add_compaction_write(struct ftl_stats_logger *logger, uint64_t bytes)
{
	if (logger) {
		logger->compaction_write_bytes += bytes;
		logger->backend_write_bytes += bytes;
	}
}

/** Add completed host-level GC relocation writes to the backend. */
static inline void
ftl_stats_logger_add_gc_write(struct ftl_stats_logger *logger, uint64_t bytes)
{
	if (logger) {
		logger->gc_write_bytes += bytes;
		logger->backend_write_bytes += bytes;
	}
}

/** Add completed metadata writes to the backend base device. */
static inline void
ftl_stats_logger_add_backend_md_write(struct ftl_stats_logger *logger, uint64_t bytes)
{
	if (logger) {
		logger->backend_md_write_bytes += bytes;
	}
}

/**
 * Set valid blocks count
 */
static inline void
ftl_stats_logger_set_valid_blocks(struct ftl_stats_logger *logger, uint64_t count)
{
	if (logger) {
		logger->valid_blocks = count;
	}
}

/**
 * Add write hit (L2P overwrite)
 */
static inline void
ftl_stats_logger_add_write_hit(struct ftl_stats_logger *logger)
{
	if (logger) {
		logger->write_hit_count++;
	}
}

/**
 * Add GC victim blocks
 */
static inline void
ftl_stats_logger_add_gc_victim(struct ftl_stats_logger *logger, uint64_t count)
{
	if (logger) {
		logger->gc_victim_blocks += count;
	}
}

/**
 * Add evict victim blocks
 */
static inline void
ftl_stats_logger_add_evict_victim(struct ftl_stats_logger *logger, uint64_t count)
{
	if (logger) {
		logger->evict_victim_blocks += count;
	}
}

/**
 * Add L2P cache write bytes (eviction/persist writeback to nv_cache)
 */
static inline void
ftl_stats_logger_add_l2p_write(struct ftl_stats_logger *logger, uint64_t bytes)
{
	if (logger) {
		logger->l2p_write_bytes += bytes;
	}
}

/**
 * Add metadata region write bytes (ftl_md writes to nv_cache)
 */
static inline void
ftl_stats_logger_add_md_write(struct ftl_stats_logger *logger, uint64_t bytes)
{
	if (logger) {
		logger->md_write_bytes += bytes;
	}
}

#ifdef __cplusplus
}
#endif

#endif /* FTL_STATS_LOGGER_H */
