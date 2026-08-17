/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#ifndef OCF_STATS_LOGGER_H
#define OCF_STATS_LOGGER_H

#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/nvme.h"
#include <ocf/ocf.h>

/* NVMe FDP Statistics Log Page (LID=0x22) */
struct nvme_fdp_stats_log {
	uint8_t hbmw[16];   /* Host Bytes with Media Written (128-bit) */
	uint8_t mbmw[16];   /* Media Bytes with Media Written (128-bit) */
	uint8_t mbe[16];    /* Media Bytes Erased (128-bit) */
	uint8_t reserved[480];
};

/* Backend vendor log page 0xC0. Bytes 24-31 contain NAND write units. */
struct ocf_backend_vendor_log {
	uint8_t data[256];
};

/**
 * OCF Stats Logger
 *
 * Periodically logs OCF cache statistics to a CSV file:
 * - Cache usage: occupancy, free, clean, dirty (4KiB blocks)
 * - Requests: read/write hits, misses, totals
 * - Blocks: core_volume_rd/wr, cache_volume_rd/wr (4KiB blocks)
 * - FDP stats: host_written_MB, media_written_MB (from NVMe FDP log page)
 */
struct ocf_stats_logger {
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

	/* OCF cache and core handles */
	ocf_cache_t ocf_cache;
	ocf_core_t ocf_core;
	char core_name[64];

	/* Start time */
	uint64_t start_time_us;

	/* Previous values for delta calculation */
	uint64_t prev_cache_wr_blocks;
	uint64_t prev_core_wr_blocks;
	uint64_t prev_host_wr_blocks;

	/* NVMe FDP stats */
	struct spdk_nvme_ctrlr *nvme_ctrlr;
	struct nvme_fdp_stats_log *log_page_buf;
	bool log_page_pending;
	uint64_t nvme_host_written;
	uint64_t nvme_media_written;
	uint64_t prev_nvme_host_written;
	uint64_t prev_nvme_media_written;

	/* Backend QLC physical NAND writes (vendor log page 0xC0). */
	struct spdk_nvme_ctrlr *backend_nvme_ctrlr;
	struct ocf_backend_vendor_log *backend_log_page_buf;
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
struct ocf_stats_logger *ocf_stats_logger_create(const char *dev_name,
						  const char *log_dir,
						  uint64_t interval_us);

/**
 * Destroy a stats logger
 *
 * @param logger Stats logger to destroy
 */
void ocf_stats_logger_destroy(struct ocf_stats_logger *logger);

/**
 * Set OCF cache and core for stats collection
 *
 * @param logger Stats logger
 * @param cache OCF cache handle
 * @param core OCF core handle
 * @param core_name Core device name
 */
void ocf_stats_logger_set_cache(struct ocf_stats_logger *logger,
				 ocf_cache_t cache,
				 ocf_core_t core,
				 const char *core_name);

/**
 * Set NVMe controller for FDP stats collection
 *
 * @param logger Stats logger
 * @param ctrlr NVMe controller handle (cache device)
 */
void ocf_stats_logger_set_nvme_ctrlr(struct ocf_stats_logger *logger,
				      struct spdk_nvme_ctrlr *ctrlr);

/** Set backend QLC NVMe controller for physical NAND write collection. */
void ocf_stats_logger_set_backend_nvme_ctrlr(struct ocf_stats_logger *logger,
					      struct spdk_nvme_ctrlr *ctrlr);

/**
 * Start the stats logger (call from SPDK thread)
 *
 * @param logger Stats logger
 * @return 0 on success, negative errno on failure
 */
int ocf_stats_logger_start(struct ocf_stats_logger *logger);

/**
 * Stop the stats logger
 *
 * @param logger Stats logger
 */
void ocf_stats_logger_stop(struct ocf_stats_logger *logger);

#endif /* OCF_STATS_LOGGER_H */
