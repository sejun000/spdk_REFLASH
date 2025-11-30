/*   SPDX-License-Identifier: BSD-3-Clause */

#include "vbdev_icache.h"

#include "spdk/stdinc.h"

#include <inttypes.h>
#include <limits.h>

#include "logcache/log_cache_wrapper.h"

#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

struct vbdev_icache {
	struct spdk_bdev		*backend_bdev;
	struct spdk_bdev_desc	*backend_desc;
	struct spdk_bdev		*cache_bdev;
	struct spdk_bdev_desc	*cache_desc;
	struct spdk_bdev		vbdev;
	struct spdk_thread	*thread;
	uint32_t		max_pending_io;
	TAILQ_ENTRY(vbdev_icache) link;
	uint64_t		max_num_blocks;
	struct log_cache_ctx	*log_ctx;
	char			*cache_type;
	char			*waf_log_path;
	char			*stat_log_path;
	double			valid_rate_threshold;
};

struct icache_io_channel {
	struct spdk_io_channel *backend_ch;
	struct spdk_io_channel *cache_ch;
};

struct icache_bdev_io {
	struct spdk_bdev_io_wait_entry wait_entry;
	struct spdk_io_channel *submit_ch;
	struct icache_io_channel *ic_ch;
	struct vbdev_icache *icache;
};

#define ICACHE_MAX_RW_BYTES	(2 * 1024 * 1024)

static TAILQ_HEAD(, vbdev_icache) g_icache_nodes = TAILQ_HEAD_INITIALIZER(g_icache_nodes);

static int vbdev_icache_init(void);
static void vbdev_icache_finish(void);
static int vbdev_icache_get_ctx_size(void);
static int vbdev_icache_config_json(struct spdk_json_write_ctx *w);
static void vbdev_icache_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

static struct spdk_bdev_module icache_if = {
	.name = "icache",
	.module_init = vbdev_icache_init,
	.module_fini = vbdev_icache_finish,
	.get_ctx_size = vbdev_icache_get_ctx_size,
	.config_json = vbdev_icache_config_json,
};

SPDK_BDEV_MODULE_REGISTER(icache, &icache_if)

static struct vbdev_icache *
vbdev_icache_find_by_name(const char *name)
{
	struct vbdev_icache *node;

	TAILQ_FOREACH(node, &g_icache_nodes, link) {
		if (strcmp(node->vbdev.name, name) == 0) {
			return node;
		}
	}

	return NULL;
}

static void
icache_device_unregister_cb(void *io_device)
{
	struct vbdev_icache *icache = io_device;

	free(icache->vbdev.name);
	free(icache);
}

static void
icache_close_desc(void *ctx)
{
	spdk_bdev_close(ctx);
}

static int
vbdev_icache_destruct(void *ctx)
{
	struct vbdev_icache *icache = ctx;

	log_cache_ctx_destroy(icache->log_ctx);

	TAILQ_REMOVE(&g_icache_nodes, icache, link);

	spdk_bdev_module_release_bdev(icache->backend_bdev);
	spdk_bdev_module_release_bdev(icache->cache_bdev);

	if (icache->thread && icache->thread != spdk_get_thread()) {
		spdk_thread_send_msg(icache->thread, icache_close_desc, icache->backend_desc);
		spdk_thread_send_msg(icache->thread, icache_close_desc, icache->cache_desc);
	} else {
		spdk_bdev_close(icache->backend_desc);
		spdk_bdev_close(icache->cache_desc);
	}

	spdk_io_device_unregister(icache, icache_device_unregister_cb);

	free(icache->cache_type);
	free(icache->waf_log_path);
	free(icache->stat_log_path);

	return 0;
}

static void
icache_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig = cb_arg;

	spdk_bdev_free_io(bdev_io);
	spdk_bdev_io_complete(orig, success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static inline size_t
icache_io_num_bytes(struct spdk_bdev_io *bdev_io)
{
	return bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
}

static void icache_queue_io(struct vbdev_icache *icache,
	struct icache_io_channel *ic_ch,
	struct spdk_bdev_io *bdev_io,
	struct spdk_io_channel *io_ch);

static void
icache_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = arg;
	struct icache_bdev_io *io_ctx = (struct icache_bdev_io *)bdev_io->driver_ctx;

	vbdev_icache_submit_request(io_ctx->submit_ch, bdev_io);
}

static void
icache_queue_io(struct vbdev_icache *icache,
		struct icache_io_channel *ic_ch,
		struct spdk_bdev_io *bdev_io,
		struct spdk_io_channel *io_ch)
{
	struct icache_bdev_io *io_ctx = (struct icache_bdev_io *)bdev_io->driver_ctx;
	int rc;

	io_ctx->submit_ch = io_ch;
	io_ctx->ic_ch = ic_ch;
	io_ctx->icache = icache;
	io_ctx->wait_entry.bdev = icache->backend_bdev;
	io_ctx->wait_entry.cb_arg = bdev_io;
	io_ctx->wait_entry.cb_fn = icache_resubmit_io;

	rc = spdk_bdev_queue_io_wait(icache->backend_bdev, ic_ch->backend_ch,
				     &io_ctx->wait_entry);
	if (rc) {
		SPDK_ERRLOG("queue io failed rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
icache_host_io_done(void *cb_arg, int status)
{
	struct spdk_bdev_io *bdev_io = cb_arg;
	struct icache_bdev_io *io_ctx = (struct icache_bdev_io *)bdev_io->driver_ctx;

	if (status == -ENOMEM) {
		icache_queue_io(io_ctx->icache, io_ctx->ic_ch, bdev_io, io_ctx->submit_ch);
		return;
	}

	if (status) {
		SPDK_ERRLOG("icache: host io failed rc=%d\n", status);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
icache_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_icache *icache = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_icache, vbdev);
	struct icache_io_channel *ic_ch = spdk_io_channel_get_ctx(ch);
	struct icache_bdev_io *io_ctx = (struct icache_bdev_io *)bdev_io->driver_ctx;
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (bdev_io->u.bdev.iovcnt <= 0) {
		SPDK_ERRLOG("read callback missing buffer\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	io_ctx->submit_ch = ch;
	io_ctx->ic_ch = ic_ch;
	io_ctx->icache = icache;

	rc = log_cache_ctx_read_async(icache->log_ctx, ic_ch->cache_ch, ic_ch->backend_ch,
				      bdev_io->u.bdev.offset_blocks,
				      bdev_io->u.bdev.iovs,
				      bdev_io->u.bdev.iovcnt,
				      icache_io_num_bytes(bdev_io),
				      icache_host_io_done, bdev_io);
	if (rc == -ENOMEM) {
		icache_queue_io(icache, ic_ch, bdev_io, ch);
	} else if (rc) {
		SPDK_ERRLOG("cache read submit failed rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_icache_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_icache *icache = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_icache, vbdev);
	struct icache_io_channel *ic_ch = spdk_io_channel_get_ctx(ch);
	struct icache_bdev_io *io_ctx = (struct icache_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;
	uint64_t end_offset = bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks;

	if (end_offset > icache->max_num_blocks) {
		SPDK_ERRLOG("icache: I/O exceeds backend capacity (end %" PRIu64 " > %" PRIu64 ")\n",
			    end_offset, icache->max_num_blocks);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	io_ctx->submit_ch = ch;
	io_ctx->ic_ch = ic_ch;
	io_ctx->icache = icache;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, icache_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = log_cache_ctx_write_async(icache->log_ctx, ic_ch->cache_ch, ic_ch->backend_ch,
					       bdev_io->u.bdev.offset_blocks,
					       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					       icache_io_num_bytes(bdev_io),
					       icache_host_io_done, bdev_io);
		if (rc == 0) {
			return;
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(icache->backend_desc, ic_ch->backend_ch,
						   bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
						   icache_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(icache->backend_desc, ic_ch->backend_ch,
					    bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
					    icache_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(icache->backend_desc, ic_ch->backend_ch,
					    bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
					    icache_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(icache->backend_desc, ic_ch->backend_ch,
				     icache_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("icache: unsupported io type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc == -ENOMEM) {
		icache_queue_io(icache, ic_ch, bdev_io, ch);
	} else if (rc) {
		SPDK_ERRLOG("icache: submit failed rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
vbdev_icache_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_icache *icache = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return false;
	default:
		break;
	}

	return spdk_bdev_io_type_supported(icache->backend_bdev, io_type);
}

static struct spdk_io_channel *
vbdev_icache_get_io_channel(void *ctx)
{
	struct vbdev_icache *icache = ctx;

	return spdk_get_io_channel(icache);
}

static int
icache_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_icache *icache = io_device;
	struct icache_io_channel *ic_ch = ctx_buf;

	ic_ch->backend_ch = spdk_bdev_get_io_channel(icache->backend_desc);
	if (ic_ch->backend_ch == NULL) {
		SPDK_ERRLOG("icache: failed to get backend IO channel\n");
		return -ENOMEM;
	}

	ic_ch->cache_ch = spdk_bdev_get_io_channel(icache->cache_desc);
	if (ic_ch->cache_ch == NULL) {
		SPDK_ERRLOG("icache: failed to get cache IO channel\n");
		spdk_put_io_channel(ic_ch->backend_ch);
		return -ENOMEM;
	}

	return 0;
}

static void
icache_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct icache_io_channel *ic_ch = ctx_buf;

	if (ic_ch->cache_ch) {
		spdk_put_io_channel(ic_ch->cache_ch);
		ic_ch->cache_ch = NULL;
	}

	if (ic_ch->backend_ch) {
		spdk_put_io_channel(ic_ch->backend_ch);
		ic_ch->backend_ch = NULL;
	}
}

static int
vbdev_icache_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_icache *icache = ctx;

	spdk_json_write_name(w, "icache");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "backend_bdev", spdk_bdev_get_name(icache->backend_bdev));
	spdk_json_write_named_string(w, "cache_bdev", spdk_bdev_get_name(icache->cache_bdev));
	spdk_json_write_named_uint32(w, "max_pending_io", icache->max_pending_io);
	if (icache->cache_type) {
		spdk_json_write_named_string(w, "cache_type", icache->cache_type);
	}
	if (icache->waf_log_path) {
		spdk_json_write_named_string(w, "waf_log_path", icache->waf_log_path);
	}
	if (icache->stat_log_path && icache->stat_log_path[0] != '\0') {
		spdk_json_write_named_string(w, "stat_log_path", icache->stat_log_path);
	}
	spdk_json_write_named_double(w, "valid_rate_threshold", icache->valid_rate_threshold);
	spdk_json_write_object_end(w);

	return 0;
}

static void
vbdev_icache_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct vbdev_icache *icache = SPDK_CONTAINEROF(bdev, struct vbdev_icache, vbdev);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_icache_create");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", icache->vbdev.name);
	spdk_json_write_named_string(w, "cache_bdev_name", spdk_bdev_get_name(icache->cache_bdev));
	spdk_json_write_named_string(w, "backend_bdev_name", spdk_bdev_get_name(icache->backend_bdev));
	spdk_json_write_named_uint32(w, "max_pending_io", icache->max_pending_io);
	if (icache->cache_type) {
		spdk_json_write_named_string(w, "cache_type", icache->cache_type);
	}
	if (icache->waf_log_path) {
		spdk_json_write_named_string(w, "waf_log_path", icache->waf_log_path);
	}
	if (icache->stat_log_path && icache->stat_log_path[0] != '\0') {
		spdk_json_write_named_string(w, "stat_log_path", icache->stat_log_path);
	}
	spdk_json_write_named_double(w, "valid_rate_threshold", icache->valid_rate_threshold);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static int
vbdev_icache_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct vbdev_icache *icache = ctx;

	return spdk_bdev_get_memory_domains(icache->backend_bdev, domains, array_size);
}

static const struct spdk_bdev_fn_table vbdev_icache_fn_table = {
	.destruct = vbdev_icache_destruct,
	.submit_request = vbdev_icache_submit_request,
	.io_type_supported = vbdev_icache_io_type_supported,
	.get_io_channel = vbdev_icache_get_io_channel,
	.dump_info_json = vbdev_icache_dump_info_json,
	.write_config_json = vbdev_icache_write_config_json,
	.get_memory_domains = vbdev_icache_get_memory_domains,
};

static void
vbdev_icache_base_hotremove(struct spdk_bdev *bdev)
{
	struct vbdev_icache *node, *tmp;

	TAILQ_FOREACH_SAFE(node, &g_icache_nodes, link, tmp) {
		if (node->backend_bdev == bdev || node->cache_bdev == bdev) {
			spdk_bdev_unregister(&node->vbdev, NULL, NULL);
		}
	}
}

static void
vbdev_icache_base_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		void *event_ctx)
{
	if (type == SPDK_BDEV_EVENT_REMOVE) {
		vbdev_icache_base_hotremove(bdev);
	} else {
		SPDK_NOTICELOG("icache ignored event type %d\n", type);
	}
}

int
vbdev_icache_create(const char *name, const char *cache_bdev_name,
	const char *backend_bdev_name, uint32_t max_pending_io,
	const char *cache_type, const char *waf_log_path,
	const char *stat_log_path, double valid_rate_threshold)
{
	struct vbdev_icache *icache;
	int rc;

	if (!name || !cache_bdev_name || !backend_bdev_name) {
		return -EINVAL;
	}

	if (vbdev_icache_find_by_name(name)) {
		return -EEXIST;
	}

	icache = calloc(1, sizeof(*icache));
	if (!icache) {
		return -ENOMEM;
	}

	icache->thread = spdk_get_thread();
	icache->max_pending_io = max_pending_io;
	icache->valid_rate_threshold = valid_rate_threshold;

	const char *type = (cache_type && cache_type[0] != '\0') ? cache_type : "LOG_GREEDY";
	const char *waf_path = (waf_log_path && waf_log_path[0] != '\0') ? waf_log_path : "/tmp/icache_waf.log";
	const char *stat_path = stat_log_path ? stat_log_path : "";

	icache->cache_type = strdup(type);
	if (!icache->cache_type) {
		rc = -ENOMEM;
		goto err_alloc_simple;
	}

	icache->waf_log_path = strdup(waf_path);
	if (!icache->waf_log_path) {
		rc = -ENOMEM;
		goto err_alloc_simple;
	}

	icache->stat_log_path = strdup(stat_path);
	if (!icache->stat_log_path) {
		rc = -ENOMEM;
		goto err_alloc_simple;
	}

	rc = spdk_bdev_open_ext(backend_bdev_name, true, vbdev_icache_base_event_cb,
		icache, &icache->backend_desc);
	if (rc) {
		SPDK_ERRLOG("open backend %s failed rc=%d\n", backend_bdev_name, rc);
		free(icache);
		return rc;
	}
	icache->backend_bdev = spdk_bdev_desc_get_bdev(icache->backend_desc);

	rc = spdk_bdev_open_ext(cache_bdev_name, true, vbdev_icache_base_event_cb,
		icache, &icache->cache_desc);
	if (rc) {
		SPDK_ERRLOG("open cache %s failed rc=%d\n", cache_bdev_name, rc);
		spdk_bdev_close(icache->backend_desc);
		free(icache);
		return rc;
	}
	icache->cache_bdev = spdk_bdev_desc_get_bdev(icache->cache_desc);

	if (icache->cache_bdev->blocklen != icache->backend_bdev->blocklen) {
		SPDK_ERRLOG("icache: cache block size %u differs from backend block size %u\n",
			    icache->cache_bdev->blocklen, icache->backend_bdev->blocklen);
		rc = -EINVAL;
		goto err_open;
	}

	icache->log_ctx = log_cache_ctx_create(icache->cache_desc, icache->backend_desc,
					       icache->cache_bdev->blockcnt,
					       icache->backend_bdev->blockcnt,
					       icache->cache_bdev->blocklen,
					       icache->cache_type,
					       icache->waf_log_path,
					       icache->stat_log_path,
					       icache->valid_rate_threshold);
	if (!icache->log_ctx) {
		rc = -ENOMEM;
		goto err_open;
	}

	icache->vbdev.name = strdup(name);
	if (!icache->vbdev.name) {
		rc = -ENOMEM;
		goto err_open;
	}
	icache->vbdev.product_name = "icache";
	icache->vbdev.module = &icache_if;
	icache->vbdev.fn_table = &vbdev_icache_fn_table;
	icache->vbdev.ctxt = icache;
	icache->vbdev.blocklen = icache->backend_bdev->blocklen;
	icache->max_num_blocks = icache->backend_bdev->blockcnt;
	icache->vbdev.blockcnt = icache->max_num_blocks;
	icache->vbdev.required_alignment = icache->backend_bdev->required_alignment;
	icache->vbdev.optimal_io_boundary = icache->backend_bdev->optimal_io_boundary;
	icache->vbdev.split_on_optimal_io_boundary = icache->backend_bdev->split_on_optimal_io_boundary;
	icache->vbdev.max_write_zeroes = icache->backend_bdev->max_write_zeroes;
	icache->vbdev.max_unmap = icache->backend_bdev->max_unmap;
	icache->vbdev.write_cache = icache->backend_bdev->write_cache;
	icache->vbdev.acwu = icache->backend_bdev->acwu;
	icache->vbdev.md_interleave = icache->backend_bdev->md_interleave;
	icache->vbdev.dif_type = icache->backend_bdev->dif_type;
	icache->vbdev.dif_is_head_of_md = icache->backend_bdev->dif_is_head_of_md;
	icache->vbdev.dif_check_flags = icache->backend_bdev->dif_check_flags;
	icache->vbdev.dif_pi_format = icache->backend_bdev->dif_pi_format;
	icache->vbdev.md_len = icache->backend_bdev->md_len;
	spdk_uuid_generate(&icache->vbdev.uuid);

	uint32_t two_mb_blocks = ICACHE_MAX_RW_BYTES / icache->vbdev.blocklen;
	if (two_mb_blocks == 0) {
		two_mb_blocks = 1;
	}
	if (icache->backend_bdev->max_rw_size != 0) {
		icache->vbdev.max_rw_size = spdk_min(icache->backend_bdev->max_rw_size, two_mb_blocks);
	} else {
		icache->vbdev.max_rw_size = two_mb_blocks;
	}

	rc = spdk_bdev_module_claim_bdev(icache->backend_bdev, icache->backend_desc,
		icache->vbdev.module);
	if (rc) {
		SPDK_ERRLOG("claim backend failed rc=%d\n", rc);
		goto err_alloc;
	}

	rc = spdk_bdev_module_claim_bdev(icache->cache_bdev, icache->cache_desc,
		icache->vbdev.module);
	if (rc) {
		SPDK_ERRLOG("claim cache failed rc=%d\n", rc);
		spdk_bdev_module_release_bdev(icache->backend_bdev);
		goto err_alloc;
	}

	spdk_io_device_register(icache, icache_ch_create_cb, icache_ch_destroy_cb,
		sizeof(struct icache_io_channel), icache->vbdev.name);

	rc = spdk_bdev_register(&icache->vbdev);
	if (rc) {
		spdk_io_device_unregister(icache, icache_device_unregister_cb);
		spdk_bdev_module_release_bdev(icache->backend_bdev);
		spdk_bdev_module_release_bdev(icache->cache_bdev);
		goto err_alloc;
	}

	TAILQ_INSERT_TAIL(&g_icache_nodes, icache, link);

	return 0;

err_alloc_simple:
	free(icache->stat_log_path);
	free(icache->waf_log_path);
	free(icache->cache_type);
	icache->stat_log_path = NULL;
	icache->waf_log_path = NULL;
	icache->cache_type = NULL;
err_alloc:
	free(icache->vbdev.name);
err_open:
	free(icache->stat_log_path);
	free(icache->waf_log_path);
	free(icache->cache_type);
	log_cache_ctx_destroy(icache->log_ctx);
	spdk_bdev_close(icache->cache_desc);
	spdk_bdev_close(icache->backend_desc);
	free(icache);
	return rc;
}

int
vbdev_icache_delete(const char *name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct vbdev_icache *icache;

	icache = vbdev_icache_find_by_name(name);
	if (!icache) {
		return -ENODEV;
	}

	spdk_bdev_unregister(&icache->vbdev, cb_fn, cb_arg);

	return 0;
}

static int
vbdev_icache_init(void)
{
	return 0;
}

static void
vbdev_icache_finish(void)
{
	struct vbdev_icache *icache;

	while ((icache = TAILQ_FIRST(&g_icache_nodes))) {
		spdk_bdev_unregister(&icache->vbdev, NULL, NULL);
	}
}

static int
vbdev_icache_get_ctx_size(void)
{
	return sizeof(struct icache_bdev_io);
}

static int
vbdev_icache_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_icache *icache;

	TAILQ_FOREACH(icache, &g_icache_nodes, link) {
		vbdev_icache_write_config_json(&icache->vbdev, w);
	}

	return 0;
}
