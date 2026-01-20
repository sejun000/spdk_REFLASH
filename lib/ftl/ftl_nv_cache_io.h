/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_NV_CACHE_IO_H
#define FTL_NV_CACHE_IO_H

#include "spdk/bdev.h"
#include "ftl_core.h"

/* FDP Placement Handles */
#define FTL_FDP_HANDLE_USER_DATA	0	/* User data (sequential) */
#define FTL_FDP_HANDLE_METADATA		1	/* Metadata (in-place overwrite) */

/* Enable FDP for metadata writes (set to 1 to enable) */
#define FTL_FDP_METADATA_ENABLED	0

static inline int
ftl_nv_cache_bdev_read_blocks_with_md(struct spdk_bdev_desc *desc,
				      struct spdk_io_channel *ch,
				      void *buf, void *md,
				      uint64_t offset_blocks, uint64_t num_blocks,
				      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (spdk_bdev_get_md_size(spdk_bdev_desc_get_bdev(desc))) {
		return spdk_bdev_read_blocks_with_md(desc, ch, buf, md ? : g_ftl_read_buf,
						     offset_blocks, num_blocks, cb, cb_arg);
	} else {
		return spdk_bdev_read_blocks(desc, ch, buf, offset_blocks, num_blocks,
					     cb, cb_arg);
	}
}

static inline int
ftl_nv_cache_bdev_write_blocks_with_md(struct spdk_bdev_desc *desc,
				       struct spdk_io_channel *ch,
				       void *buf, void *md,
				       uint64_t offset_blocks, uint64_t num_blocks,
				       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (spdk_bdev_get_md_size(spdk_bdev_desc_get_bdev(desc))) {
		return spdk_bdev_write_blocks_with_md(desc, ch, buf, md ? : g_ftl_write_buf,
						      offset_blocks, num_blocks, cb, cb_arg);
	} else {
		return spdk_bdev_write_blocks(desc, ch, buf, offset_blocks, num_blocks,
					      cb, cb_arg);
	}
}

/*
 * FDP-enabled write function for metadata
 * Uses FDP placement handle to separate metadata from user data on SSD
 */
static inline int
ftl_nv_cache_bdev_write_blocks_with_md_fdp(struct spdk_bdev_desc *desc,
					   struct spdk_io_channel *ch,
					   void *buf, void *md,
					   uint64_t offset_blocks, uint64_t num_blocks,
					   spdk_bdev_io_completion_cb cb, void *cb_arg,
					   uint16_t placement_handle)
{
#if FTL_FDP_METADATA_ENABLED
	struct spdk_bdev_ext_io_opts opts = {};
	opts.size = sizeof(opts);
	opts.nvme_cdw12.write.dtype = 2;  /* Directive Type = FDP */
	opts.nvme_cdw13.write.dspec = placement_handle;

	if (spdk_bdev_get_md_size(spdk_bdev_desc_get_bdev(desc))) {
		struct iovec iov = { .iov_base = buf, .iov_len = num_blocks * FTL_BLOCK_SIZE };
		return spdk_bdev_writev_blocks_with_md_ext(desc, ch, &iov, 1,
							   md ? : g_ftl_write_buf,
							   offset_blocks, num_blocks,
							   cb, cb_arg, &opts);
	} else {
		struct iovec iov = { .iov_base = buf, .iov_len = num_blocks * FTL_BLOCK_SIZE };
		return spdk_bdev_writev_blocks_ext(desc, ch, &iov, 1,
						   offset_blocks, num_blocks,
						   cb, cb_arg, &opts);
	}
#else
	/* FDP disabled, use regular write */
	return ftl_nv_cache_bdev_write_blocks_with_md(desc, ch, buf, md,
						      offset_blocks, num_blocks,
						      cb, cb_arg);
#endif
}

#endif /* FTL_NV_CACHE_IO_H */
