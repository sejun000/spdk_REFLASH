/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "ocf_stats_logger.h"
#include "stats.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <string.h>

#include "spdk/env.h"
#include "spdk/log.h"

/* Default logging interval: 2 seconds */
#define OCF_STATS_LOGGER_DEFAULT_INTERVAL_US 2000000

/* Default log directory */
#define OCF_STATS_LOGGER_DEFAULT_DIR "logging"

/* NVMe FDP Statistics Log Page ID (from NVMe spec) */
#define NVME_LOG_LID_FDP_STATS 0x22

static void
ocf_stats_logger_log_page_done(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct ocf_stats_logger *logger = cb_arg;
	logger->log_page_pending = false;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_WARNLOG("OCF StatsLogger: FDP stats log page read failed, sct=%d sc=%d\n",
			     cpl->status.sct, cpl->status.sc);
		return;
	}

	/* Successfully read FDP stats log page - extract values */
	/* HBMW and MBMW are 128-bit little-endian values in bytes */
	/* We only need the lower 64 bits (up to 16 exabytes is enough) */
	uint64_t hbmw_lo, mbmw_lo;
	memcpy(&hbmw_lo, logger->log_page_buf->hbmw, sizeof(uint64_t));
	memcpy(&mbmw_lo, logger->log_page_buf->mbmw, sizeof(uint64_t));
	logger->nvme_host_written = hbmw_lo;
	logger->nvme_media_written = mbmw_lo;
}

static void
ocf_stats_logger_read_log_page(struct ocf_stats_logger *logger)
{
	uint16_t egid;
	uint32_t cdw11, cdw14;
	int rc;

	if (!logger->nvme_ctrlr || !logger->log_page_buf || logger->log_page_pending) {
		return;
	}

	/* Read FDP Statistics Log Page (LID=0x22) */
	/* CDW11 bits 31:16 = LSI (Log Specific Identifier) = Endurance Group ID */
	/* CDW14 bits 31:24 = CSI (Command Set Identifier) = 0 (NVM) */
	egid = 1;  /* Endurance Group ID */
	cdw11 = (uint32_t)egid << 16;  /* LSI in bits 31:16 */
	cdw14 = 0;  /* CSI=0 (NVM) in bits 31:24 */

	rc = spdk_nvme_ctrlr_cmd_get_log_page_ext(
		logger->nvme_ctrlr,
		NVME_LOG_LID_FDP_STATS,
		0,  /* nsid = 0 */
		logger->log_page_buf,
		sizeof(struct nvme_fdp_stats_log),
		0,      /* offset */
		0,      /* cdw10 (handled internally) */
		cdw11,  /* LSI (Endurance Group ID) in bits 31:16 */
		cdw14,  /* CSI in bits 31:24 */
		ocf_stats_logger_log_page_done,
		logger);

	if (rc == 0) {
		logger->log_page_pending = true;
	}
}

static void
ocf_stats_logger_backend_log_page_done(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct ocf_stats_logger *logger = cb_arg;
	uint64_t nand_units = 0;
	uint64_t new_value;

	logger->backend_log_page_pending = false;
	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_WARNLOG("OCF StatsLogger: backend vendor log page 0xC0 read failed, sct=%d sc=%d\n",
			     cpl->status.sct, cpl->status.sc);
		return;
	}

	memcpy(&nand_units, &logger->backend_log_page_buf->data[24], sizeof(nand_units));
	new_value = nand_units * 512000ULL;
	if (logger->backend_nand_written == 0 && new_value != 0) {
		logger->prev_backend_nand_written = new_value;
	}
	logger->backend_nand_written = new_value;
}

static void
ocf_stats_logger_read_backend_log_page(struct ocf_stats_logger *logger)
{
	int rc;

	if (!logger->backend_nvme_ctrlr || !logger->backend_log_page_buf ||
	    logger->backend_log_page_pending) {
		return;
	}

	rc = spdk_nvme_ctrlr_cmd_get_log_page(
		logger->backend_nvme_ctrlr,
		0xC0,
		SPDK_NVME_GLOBAL_NS_TAG,
		logger->backend_log_page_buf,
		sizeof(struct ocf_backend_vendor_log),
		0,
		ocf_stats_logger_backend_log_page_done,
		logger);
	if (rc == 0) {
		logger->backend_log_page_pending = true;
	}
}

static void
ocf_stats_logger_write_stats(struct ocf_stats_logger *logger)
{
	struct vbdev_ocf_stats stats;
	uint64_t now_us;
	double elapsed_sec;
	int rc;

	/* 4KiB block size used by OCF */
	const double BLOCK_SIZE_KB = 4.0;
	const double MB = 1024.0 * 1024.0;

	if (!logger->log_fp || !logger->ocf_cache) {
		return;
	}

	/* Get OCF stats */
	rc = vbdev_ocf_stats_get(logger->ocf_cache, logger->core_name, &stats);
	if (rc != 0) {
		return;
	}

	/* Calculate elapsed time */
	now_us = spdk_get_ticks() * 1000000 / spdk_get_ticks_hz();
	elapsed_sec = (now_us - logger->start_time_us) / 1000000.0;

	/* Current values (in 4KiB blocks) */
	uint64_t cache_wr_blocks = stats.blocks.cache_volume_wr.value;
	uint64_t core_wr_blocks = stats.blocks.core_volume_wr.value;
	uint64_t host_wr_blocks = stats.blocks.volume_wr.value;

	/* Calculate deltas */
	uint64_t cache_wr_delta = cache_wr_blocks - logger->prev_cache_wr_blocks;
	uint64_t core_wr_delta = core_wr_blocks - logger->prev_core_wr_blocks;
	uint64_t host_wr_delta = host_wr_blocks - logger->prev_host_wr_blocks;

	/* NVMe FDP deltas (already in bytes from FDP Statistics Log) */
	uint64_t nvme_host_delta = logger->nvme_host_written - logger->prev_nvme_host_written;
	uint64_t nvme_media_delta = logger->nvme_media_written - logger->prev_nvme_media_written;
	uint64_t backend_nand_delta = logger->backend_nand_written - logger->prev_backend_nand_written;

	/* Update previous values */
	logger->prev_cache_wr_blocks = cache_wr_blocks;
	logger->prev_core_wr_blocks = core_wr_blocks;
	logger->prev_host_wr_blocks = host_wr_blocks;
	logger->prev_nvme_host_written = logger->nvme_host_written;
	logger->prev_nvme_media_written = logger->nvme_media_written;
	logger->prev_backend_nand_written = logger->backend_nand_written;

	/* Write log line:
	 * time_sec, occupancy_blocks, free_blocks, clean_blocks, dirty_blocks,
	 * rd_hits, rd_misses, rd_total, wr_hits, wr_misses, wr_total,
	 * cache_rd_blocks, cache_wr_blocks, core_rd_blocks, core_wr_blocks,
	 * host_rd_blocks, host_wr_blocks,
	 * cache_wr_delta, core_wr_delta, host_wr_delta,
	 * cache_wr_MB, core_wr_MB, host_wr_MB,
	 * nvme_host_MB, nvme_media_MB, nvme_host_delta_MB, nvme_media_delta_MB
	 */
	fprintf(logger->log_fp,
		"%.2f,%lu,%lu,%lu,%lu,"
		"%lu,%lu,%lu,%lu,%lu,%lu,"
		"%lu,%lu,%lu,%lu,%lu,%lu,"
		"%lu,%lu,%lu,"
		"%.2f,%.2f,%.2f,"
		"%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
		elapsed_sec,
		stats.usage.occupancy.value,
		stats.usage.free.value,
		stats.usage.clean.value,
		stats.usage.dirty.value,
		stats.reqs.rd_hits.value,
		stats.reqs.rd_partial_misses.value + stats.reqs.rd_full_misses.value,
		stats.reqs.rd_total.value,
		stats.reqs.wr_hits.value,
		stats.reqs.wr_partial_misses.value + stats.reqs.wr_full_misses.value,
		stats.reqs.wr_total.value,
		stats.blocks.cache_volume_rd.value,
		cache_wr_blocks,
		stats.blocks.core_volume_rd.value,
		core_wr_blocks,
		stats.blocks.volume_rd.value,
		host_wr_blocks,
		cache_wr_delta,
		core_wr_delta,
		host_wr_delta,
		cache_wr_blocks * BLOCK_SIZE_KB / 1024.0,  /* Convert to MB */
		core_wr_blocks * BLOCK_SIZE_KB / 1024.0,
		host_wr_blocks * BLOCK_SIZE_KB / 1024.0,
		logger->nvme_host_written / MB,
		logger->nvme_media_written / MB,
		nvme_host_delta / MB,
		nvme_media_delta / MB,
		logger->backend_nand_written / MB,
		backend_nand_delta / MB);
	fflush(logger->log_fp);
}

static int
ocf_stats_logger_poller_fn(void *arg)
{
	struct ocf_stats_logger *logger = arg;

	/* Process admin completions to receive FDP log page results */
	if (logger->nvme_ctrlr) {
		spdk_nvme_ctrlr_process_admin_completions(logger->nvme_ctrlr);
	}
	if (logger->backend_nvme_ctrlr && logger->backend_nvme_ctrlr != logger->nvme_ctrlr) {
		spdk_nvme_ctrlr_process_admin_completions(logger->backend_nvme_ctrlr);
	}

	/* Try to read NVMe FDP log page (async) */
	ocf_stats_logger_read_log_page(logger);
	ocf_stats_logger_read_backend_log_page(logger);

	/* Log current stats */
	ocf_stats_logger_write_stats(logger);

	return SPDK_POLLER_BUSY;
}

struct ocf_stats_logger *
ocf_stats_logger_create(const char *dev_name, const char *log_dir, uint64_t interval_us)
{
	struct ocf_stats_logger *logger;

	logger = calloc(1, sizeof(*logger));
	if (!logger) {
		SPDK_ERRLOG("OCF StatsLogger: failed to allocate memory\n");
		return NULL;
	}

	logger->dev_name = strdup(dev_name ? dev_name : "ocf");
	if (!logger->dev_name) {
		free(logger);
		return NULL;
	}

	snprintf(logger->log_dir, sizeof(logger->log_dir), "%s",
		 log_dir ? log_dir : OCF_STATS_LOGGER_DEFAULT_DIR);

	logger->interval_us = interval_us > 0 ? interval_us : OCF_STATS_LOGGER_DEFAULT_INTERVAL_US;

	return logger;
}

void
ocf_stats_logger_destroy(struct ocf_stats_logger *logger)
{
	if (!logger) {
		return;
	}

	ocf_stats_logger_stop(logger);

	if (logger->log_page_buf) {
		spdk_dma_free(logger->log_page_buf);
		logger->log_page_buf = NULL;
	}
	if (logger->backend_log_page_buf) {
		spdk_dma_free(logger->backend_log_page_buf);
		logger->backend_log_page_buf = NULL;
	}

	free(logger->dev_name);
	free(logger);
}

void
ocf_stats_logger_set_cache(struct ocf_stats_logger *logger,
			    ocf_cache_t cache,
			    ocf_core_t core,
			    const char *core_name)
{
	if (!logger) {
		return;
	}

	logger->ocf_cache = cache;
	logger->ocf_core = core;
	if (core_name) {
		snprintf(logger->core_name, sizeof(logger->core_name), "%s", core_name);
	}
	SPDK_NOTICELOG("OCF StatsLogger: set cache %p, core %p, core_name=%s\n",
		       cache, core, logger->core_name);
}

void
ocf_stats_logger_set_nvme_ctrlr(struct ocf_stats_logger *logger,
				 struct spdk_nvme_ctrlr *ctrlr)
{
	if (!logger) {
		return;
	}

	logger->nvme_ctrlr = ctrlr;
	SPDK_NOTICELOG("OCF StatsLogger: set_nvme_ctrlr called with ctrlr=%p\n", ctrlr);

	/* Allocate DMA buffer for NVMe FDP stats log page if not already allocated */
	if (ctrlr && !logger->log_page_buf) {
		logger->log_page_buf = spdk_dma_zmalloc(sizeof(struct nvme_fdp_stats_log), 4096, NULL);
		if (logger->log_page_buf) {
			SPDK_NOTICELOG("OCF StatsLogger: allocated FDP stats log page buffer (%zu bytes)\n",
				       sizeof(struct nvme_fdp_stats_log));
		} else {
			SPDK_WARNLOG("OCF StatsLogger: failed to allocate log page buffer, NVMe FDP stats disabled\n");
		}
	}
}

void
ocf_stats_logger_set_backend_nvme_ctrlr(struct ocf_stats_logger *logger,
					 struct spdk_nvme_ctrlr *ctrlr)
{
	if (!logger) {
		return;
	}

	logger->backend_nvme_ctrlr = ctrlr;
	SPDK_NOTICELOG("OCF StatsLogger: set backend NVMe controller %p\n", ctrlr);
	if (ctrlr && !logger->backend_log_page_buf) {
		logger->backend_log_page_buf = spdk_dma_zmalloc(
			sizeof(struct ocf_backend_vendor_log), 4096, NULL);
		if (!logger->backend_log_page_buf) {
			SPDK_WARNLOG("OCF StatsLogger: failed to allocate backend log page buffer\n");
		}
	}
}

int
ocf_stats_logger_start(struct ocf_stats_logger *logger)
{
	struct stat st;
	time_t now;
	struct tm *tm_info;
	char timestamp[32];

	if (!logger || logger->started) {
		return logger ? 0 : -EINVAL;
	}

	if (!logger->ocf_cache) {
		SPDK_ERRLOG("OCF StatsLogger: cache not set\n");
		return -EINVAL;
	}

	/* Create log directory if not exists */
	if (stat(logger->log_dir, &st) != 0) {
		if (mkdir(logger->log_dir, 0755) != 0 && errno != EEXIST) {
			SPDK_ERRLOG("OCF StatsLogger: failed to create directory %s: %s\n",
				    logger->log_dir, strerror(errno));
			return -errno;
		}
	}

	/* Generate log filename with timestamp */
	now = time(NULL);
	tm_info = localtime(&now);
	strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

	snprintf(logger->log_path, sizeof(logger->log_path), "%s/%s_%s.csv",
		 logger->log_dir, logger->dev_name, timestamp);

	/* Open log file */
	logger->log_fp = fopen(logger->log_path, "w");
	if (!logger->log_fp) {
		SPDK_ERRLOG("OCF StatsLogger: failed to open log file %s: %s\n",
			    logger->log_path, strerror(errno));
		return -errno;
	}

	/* Write CSV header */
	fprintf(logger->log_fp,
		"time_sec,occupancy_blocks,free_blocks,clean_blocks,dirty_blocks,"
		"rd_hits,rd_misses,rd_total,wr_hits,wr_misses,wr_total,"
		"cache_rd_blocks,cache_wr_blocks,core_rd_blocks,core_wr_blocks,"
		"host_rd_blocks,host_wr_blocks,"
		"cache_wr_delta,core_wr_delta,host_wr_delta,"
		"cache_wr_MB,core_wr_MB,host_wr_MB,"
		"nvme_host_MB,nvme_media_MB,nvme_host_delta_MB,nvme_media_delta_MB,"
		"backend_nand_written_MB,backend_nand_delta_MB\n");
	fflush(logger->log_fp);

	/* Record start time */
	logger->start_time_us = spdk_get_ticks() * 1000000 / spdk_get_ticks_hz();

	/* Register SPDK poller */
	logger->poller = spdk_poller_register(ocf_stats_logger_poller_fn, logger,
					       logger->interval_us);
	if (!logger->poller) {
		SPDK_ERRLOG("OCF StatsLogger: failed to register poller\n");
		fclose(logger->log_fp);
		logger->log_fp = NULL;
		return -ENOMEM;
	}

	logger->started = true;
	SPDK_NOTICELOG("OCF StatsLogger: started logging to %s (interval=%lu us)\n",
		       logger->log_path, logger->interval_us);

	return 0;
}

void
ocf_stats_logger_stop(struct ocf_stats_logger *logger)
{
	if (!logger || !logger->started) {
		return;
	}

	if (logger->poller) {
		spdk_poller_unregister(&logger->poller);
		logger->poller = NULL;
	}

	/* Write final stats */
	if (logger->log_fp) {
		ocf_stats_logger_write_stats(logger);
		fclose(logger->log_fp);
		logger->log_fp = NULL;
	}

	logger->started = false;
	SPDK_NOTICELOG("OCF StatsLogger: stopped\n");
}
