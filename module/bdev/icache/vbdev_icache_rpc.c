/*   SPDX-License-Identifier: BSD-3-Clause */

#include "vbdev_icache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "spdk/json.h"
#include "spdk/rpc.h"
#include "spdk/string.h"

struct rpc_icache_create {
	char *name;
	char *cache_bdev_name;
	char *backend_bdev_name;
	uint32_t max_pending_io;
	char *cache_type;
	char *waf_log_path;
	char *stat_log_path;
	double valid_rate_threshold;
	bool backend_dsm_enabled;
};

static void
free_rpc_icache_create(struct rpc_icache_create *req)
{
	free(req->name);
	free(req->cache_bdev_name);
	free(req->backend_bdev_name);
	free(req->cache_type);
	free(req->waf_log_path);
	free(req->stat_log_path);
}

static int
decode_double(const struct spdk_json_val *val, void *out)
{
	double *parsed = out;
	char buf[64];
	char *end = NULL;
	double value;

	if (val->type == SPDK_JSON_VAL_NUMBER) {
		/* JSON number: use the text representation directly */
		if (val->len >= sizeof(buf)) {
			return -EINVAL;
		}
		memcpy(buf, val->start, val->len);
		buf[val->len] = '\0';

		errno = 0;
		value = strtod(buf, &end);
		if (errno != 0 || end == buf || *end != '\0') {
			return -EINVAL;
		}
	} else if (val->type == SPDK_JSON_VAL_STRING) {
		/* JSON string: decode and parse */
		char *tmp = NULL;
		int rc = spdk_json_decode_string(val, &tmp);
		if (rc) {
			return rc;
		}

		errno = 0;
		value = strtod(tmp, &end);
		free(tmp);
		if (errno != 0 || end == tmp) {
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	*parsed = value;
	return 0;
}

static const struct spdk_json_object_decoder rpc_icache_create_decoders[] = {
	{"name", offsetof(struct rpc_icache_create, name), spdk_json_decode_string},
	{"cache_bdev_name", offsetof(struct rpc_icache_create, cache_bdev_name), spdk_json_decode_string},
	{"backend_bdev_name", offsetof(struct rpc_icache_create, backend_bdev_name), spdk_json_decode_string},
	{"max_pending_io", offsetof(struct rpc_icache_create, max_pending_io), spdk_json_decode_uint32, true},
	{"cache_type", offsetof(struct rpc_icache_create, cache_type), spdk_json_decode_string, true},
	{"waf_log_path", offsetof(struct rpc_icache_create, waf_log_path), spdk_json_decode_string, true},
	{"stat_log_path", offsetof(struct rpc_icache_create, stat_log_path), spdk_json_decode_string, true},
	{"valid_rate_threshold", offsetof(struct rpc_icache_create, valid_rate_threshold), decode_double, true},
	{"backend_dsm_enabled", offsetof(struct rpc_icache_create, backend_dsm_enabled), spdk_json_decode_bool, true},
};

static void
rpc_bdev_icache_create(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_icache_create req = {};
	struct spdk_json_write_ctx *w;
	int rc;

	req.max_pending_io = 64;
	req.valid_rate_threshold = 0.0;
	req.backend_dsm_enabled = true;

	if (spdk_json_decode_object(params, rpc_icache_create_decoders,
		SPDK_COUNTOF(rpc_icache_create_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
			"Invalid parameters");
		free_rpc_icache_create(&req);
		return;
	}

	rc = vbdev_icache_create(req.name, req.cache_bdev_name,
		req.backend_bdev_name, req.max_pending_io,
		req.cache_type, req.waf_log_path, req.stat_log_path,
		req.valid_rate_threshold, req.backend_dsm_enabled);
	free_rpc_icache_create(&req);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
			"icache create failed (%d)", rc);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

struct rpc_icache_delete {
	char *name;
};

static const struct spdk_json_object_decoder rpc_icache_delete_decoders[] = {
	{"name", offsetof(struct rpc_icache_delete, name), spdk_json_decode_string},
};

static void
rpc_bdev_icache_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (bdeverrno) {
		spdk_jsonrpc_send_error_response_fmt(request, bdeverrno,
			"icache delete failed (%d)", bdeverrno);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
rpc_bdev_icache_delete(struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *params)
{
	struct rpc_icache_delete req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_icache_delete_decoders,
		SPDK_COUNTOF(rpc_icache_delete_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
			"Invalid parameters");
		return;
	}

	rc = vbdev_icache_delete(req.name, rpc_bdev_icache_delete_cb, request);
	free(req.name);
	if (rc) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
			"icache delete failed (%d)", rc);
	}
}

SPDK_RPC_REGISTER("bdev_icache_create", rpc_bdev_icache_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER("bdev_icache_delete", rpc_bdev_icache_delete, SPDK_RPC_RUNTIME)
