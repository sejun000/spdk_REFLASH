/*   SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"

struct spdk_json_write_ctx;
struct spdk_jsonrpc_request;

int vbdev_icache_create(const char *name,
	const char *cache_bdev_name,
	const char *backend_bdev_name,
	uint32_t max_pending_io,
	const char *cache_type,
	const char *waf_log_path,
	const char *stat_log_path,
	double valid_rate_threshold);
int vbdev_icache_delete(const char *name,
	spdk_bdev_unregister_cb cb_fn,
	void *cb_arg);
