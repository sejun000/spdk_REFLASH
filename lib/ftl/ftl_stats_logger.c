/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electronics Co., Ltd.
 *   All rights reserved.
 */

#include "ftl_stats_logger.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <string.h>

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"

/* NVMe Log Page ID for FDP Statistics (from NVMe spec) */
#define NVME_LOG_LID_FDP_STATS 0x22

/* Default logging interval: 2 seconds */
#define FTL_STATS_LOGGER_DEFAULT_INTERVAL_US 2000000

/* Default log directory */
#define FTL_STATS_LOGGER_DEFAULT_DIR "logging"

static void
ftl_stats_logger_log_page_done(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct ftl_stats_logger *logger = cb_arg;

	logger->log_page_pending = false;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_WARNLOG("FTL StatsLogger: NVMe FDP stats log page read failed, sct=%d sc=%d\n",
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
ftl_stats_logger_read_nvme_log_page(struct ftl_stats_logger *logger)
{
	int rc;
	uint16_t egid;
	uint32_t cdw11;
	uint32_t cdw14;

	if (!logger->nvme_ctrlr || !logger->log_page_buf || logger->log_page_pending) {
		return;
	}

	/* Read FDP Statistics Log Page (LID=0x22)
	 * Based on nvme-cli implementation:
	 * - CDW11 bits 31:16 = LSI (Log Specific Identifier) = Endurance Group ID
	 * - CDW14 bits 31:24 = CSI (Command Set Identifier) = 0 (NVM)
	 * - nsid = 0
	 */
	egid = 1;  /* Endurance Group ID */
	cdw11 = (uint32_t)egid << 16;  /* LSI in bits 31:16 */
	cdw14 = 0;  /* CSI=0 (NVM) in bits 31:24 */

	rc = spdk_nvme_ctrlr_cmd_get_log_page_ext(
		logger->nvme_ctrlr,
		NVME_LOG_LID_FDP_STATS,
		0,  /* nsid = 0 */
		logger->log_page_buf,
		sizeof(struct ftl_nvme_fdp_stats_log),
		0,      /* offset */
		0,      /* cdw10 (handled internally) */
		cdw11,  /* LSI (Endurance Group ID) in bits 31:16 */
		cdw14,  /* CSI in bits 31:24 */
		ftl_stats_logger_log_page_done,
		logger);

	if (rc == 0) {
		logger->log_page_pending = true;
	}
}

static void
ftl_stats_logger_write_stats(struct ftl_stats_logger *logger)
{
	uint64_t host_write, cache_write, backend_write;
	uint64_t host_delta, cache_delta, backend_delta;
	uint64_t nvme_host_delta, nvme_media_delta;
	uint64_t now_us;
	double elapsed_sec;
	const double MB = 1024.0 * 1024.0;

	if (!logger->log_fp) {
		return;
	}

	/* Get current values */
	host_write = logger->host_write_bytes;
	cache_write = logger->cache_write_bytes;
	backend_write = logger->backend_write_bytes;

	/* Calculate deltas */
	host_delta = host_write - logger->prev_host_write;
	cache_delta = cache_write - logger->prev_cache_write;
	backend_delta = backend_write - logger->prev_backend_write;

	/* NVMe FDP deltas (already in bytes from FDP Statistics Log) */
	nvme_host_delta = logger->nvme_host_written - logger->prev_nvme_host_written;
	nvme_media_delta = logger->nvme_media_written - logger->prev_nvme_media_written;

	/* Update previous values */
	logger->prev_host_write = host_write;
	logger->prev_cache_write = cache_write;
	logger->prev_backend_write = backend_write;
	logger->prev_nvme_host_written = logger->nvme_host_written;
	logger->prev_nvme_media_written = logger->nvme_media_written;

	/* Calculate elapsed time */
	now_us = spdk_get_ticks() * 1000000 / spdk_get_ticks_hz();
	elapsed_sec = (now_us - logger->start_time_us) / 1000000.0;

	/* Write log line: time, host, cache, backend, host_delta, cache_delta, backend_delta,
	 *                 nvme_host (HBMW), nvme_media (MBMW), nvme_host_delta, nvme_media_delta
	 * FDP stats (hbmw, mbmw) are already in bytes, just convert to MB */
	fprintf(logger->log_fp,
		"%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
		elapsed_sec,
		host_write / MB,
		cache_write / MB,
		backend_write / MB,
		host_delta / MB,
		cache_delta / MB,
		backend_delta / MB,
		logger->nvme_host_written / MB,
		logger->nvme_media_written / MB,
		nvme_host_delta / MB,
		nvme_media_delta / MB);
	fflush(logger->log_fp);
}

static int
ftl_stats_logger_poller_fn(void *arg)
{
	struct ftl_stats_logger *logger = arg;

	/* Process admin completions to receive FDP log page results */
	if (logger->nvme_ctrlr) {
		spdk_nvme_ctrlr_process_admin_completions(logger->nvme_ctrlr);
	}

	/* Try to read NVMe log page (async) */
	ftl_stats_logger_read_nvme_log_page(logger);

	/* Log current stats */
	ftl_stats_logger_write_stats(logger);

	return SPDK_POLLER_BUSY;
}

struct ftl_stats_logger *
ftl_stats_logger_create(const char *dev_name, const char *log_dir, uint64_t interval_us)
{
	struct ftl_stats_logger *logger;

	logger = calloc(1, sizeof(*logger));
	if (!logger) {
		SPDK_ERRLOG("FTL StatsLogger: failed to allocate memory\n");
		return NULL;
	}

	logger->dev_name = strdup(dev_name ? dev_name : "ftl");
	if (!logger->dev_name) {
		free(logger);
		return NULL;
	}

	snprintf(logger->log_dir, sizeof(logger->log_dir), "%s",
		 log_dir ? log_dir : FTL_STATS_LOGGER_DEFAULT_DIR);

	logger->interval_us = interval_us > 0 ? interval_us : FTL_STATS_LOGGER_DEFAULT_INTERVAL_US;

	return logger;
}

void
ftl_stats_logger_destroy(struct ftl_stats_logger *logger)
{
	if (!logger) {
		return;
	}

	ftl_stats_logger_stop(logger);

	if (logger->log_page_buf) {
		spdk_dma_free(logger->log_page_buf);
	}

	free(logger->dev_name);
	free(logger);
}

void
ftl_stats_logger_set_cache_bdev(struct ftl_stats_logger *logger,
				 struct spdk_bdev_desc *bdev_desc,
				 struct spdk_io_channel *ioch)
{
	if (!logger) {
		return;
	}

	logger->cache_bdev_desc = bdev_desc;
	logger->cache_ioch = ioch;
}

void
ftl_stats_logger_set_nvme_ctrlr(struct ftl_stats_logger *logger,
				 struct spdk_nvme_ctrlr *ctrlr)
{
	if (!logger) {
		return;
	}

	logger->nvme_ctrlr = ctrlr;
	SPDK_NOTICELOG("FTL StatsLogger: set NVMe controller %p\n", ctrlr);
}

int
ftl_stats_logger_start(struct ftl_stats_logger *logger)
{
	struct stat st;
	time_t now;
	struct tm *tm_info;
	char timestamp[32];

	if (!logger || logger->started) {
		return logger ? 0 : -EINVAL;
	}

	/* Create log directory if not exists */
	if (stat(logger->log_dir, &st) != 0) {
		if (mkdir(logger->log_dir, 0755) != 0 && errno != EEXIST) {
			SPDK_ERRLOG("FTL StatsLogger: failed to create directory %s: %s\n",
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
		SPDK_ERRLOG("FTL StatsLogger: failed to open log file %s: %s\n",
			    logger->log_path, strerror(errno));
		return -errno;
	}

	/* Allocate DMA buffer for NVMe log page */
	if (logger->cache_bdev_desc && !logger->log_page_buf) {
		logger->log_page_buf = spdk_dma_zmalloc(sizeof(struct ftl_nvme_fdp_stats_log),
							 4096, NULL);
		if (!logger->log_page_buf) {
			SPDK_WARNLOG("FTL StatsLogger: failed to allocate log page buffer\n");
		}
	}

	/* Write CSV header */
	fprintf(logger->log_fp,
		"time_sec,host_write_MB,cache_write_MB,backend_write_MB,"
		"host_delta_MB,cache_delta_MB,backend_delta_MB,"
		"nvme_host_written_MB,nvme_media_written_MB,"
		"nvme_host_delta_MB,nvme_media_delta_MB\n");
	fflush(logger->log_fp);

	/* Record start time */
	logger->start_time_us = spdk_get_ticks() * 1000000 / spdk_get_ticks_hz();

	/* Register SPDK poller */
	logger->poller = spdk_poller_register(ftl_stats_logger_poller_fn, logger,
					       logger->interval_us);
	if (!logger->poller) {
		SPDK_ERRLOG("FTL StatsLogger: failed to register poller\n");
		fclose(logger->log_fp);
		logger->log_fp = NULL;
		return -ENOMEM;
	}

	logger->started = true;
	SPDK_NOTICELOG("FTL StatsLogger: started logging to %s (interval=%lu us)\n",
		       logger->log_path, logger->interval_us);

	return 0;
}

void
ftl_stats_logger_stop(struct ftl_stats_logger *logger)
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
		ftl_stats_logger_write_stats(logger);
		fclose(logger->log_fp);
		logger->log_fp = NULL;
	}

	logger->started = false;
	SPDK_NOTICELOG("FTL StatsLogger: stopped\n");
}
