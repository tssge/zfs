// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2024 by OpenZFS. All rights reserved.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <endian.h>

#include <libzfs.h>
#include <libzfs_core.h>
#include <sys/dmu.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio.h>
#include <sys/fs/zfs.h>
#include <zfs_fletcher.h>

#include "zstream.h"

/*
 * Basic ext2 filesystem structures
 * Based on the ext2 filesystem specification
 */

#define	EXT2_SUPER_MAGIC	0xEF53
#define	EXT2_BLOCK_SIZE_BASE	1024
#define	EXT2_MIN_BLOCK_SIZE	1024
#define	EXT2_MAX_BLOCK_SIZE	4096
#define	EXT2_ROOT_INODE		2

/* Inode types */
#define	EXT2_S_IFREG	0x8000	/* Regular file */
#define	EXT2_S_IFDIR	0x4000	/* Directory */
#define	EXT2_S_IFLNK	0xA000	/* Symbolic link */

/* File type constants for directory entries */
#define	EXT2_FT_UNKNOWN		0
#define	EXT2_FT_REG_FILE	1
#define	EXT2_FT_DIR		2
#define	EXT2_FT_CHRDEV		3
#define	EXT2_FT_BLKDEV		4
#define	EXT2_FT_FIFO		5
#define	EXT2_FT_SOCK		6
#define	EXT2_FT_SYMLINK		7

/* Directory entry structure */
typedef struct ext2_dir_entry {
	uint32_t inode;		/* Inode number */
	uint16_t rec_len;	/* Directory entry length */
	uint8_t name_len;	/* Name length */
	uint8_t file_type;	/* File type */
	char name[];		/* File name */
} ext2_dir_entry_t;

/* Superblock structure (simplified) */
typedef struct ext2_superblock {
	uint32_t s_inodes_count;
	uint32_t s_blocks_count;
	uint32_t s_r_blocks_count;
	uint32_t s_free_blocks_count;
	uint32_t s_free_inodes_count;
	uint32_t s_first_data_block;
	uint32_t s_log_block_size;
	uint32_t s_log_frag_size;
	uint32_t s_blocks_per_group;
	uint32_t s_frags_per_group;
	uint32_t s_inodes_per_group;
	uint32_t s_mtime;
	uint32_t s_wtime;
	uint16_t s_mnt_count;
	uint16_t s_max_mnt_count;
	uint16_t s_magic;
	uint16_t s_state;
	uint16_t s_errors;
	uint16_t s_minor_rev_level;
	uint32_t s_lastcheck;
	uint32_t s_checkinterval;
	uint32_t s_creator_os;
	uint32_t s_rev_level;
	uint16_t s_def_resuid;
	uint16_t s_def_resgid;
	/* Extended fields for ext2 revision 1 */
	uint32_t s_first_ino;
	uint16_t s_inode_size;
	uint16_t s_block_group_nr;
	uint32_t s_feature_compat;
	uint32_t s_feature_incompat;
	uint32_t s_feature_ro_compat;
	uint8_t s_uuid[16];
	char s_volume_name[16];
	char s_last_mounted[64];
	uint32_t s_algorithm_usage_bitmap;
} __attribute__((packed)) ext2_superblock_t;

/* Inode structure (simplified) */
typedef struct ext2_inode {
	uint16_t i_mode;
	uint16_t i_uid;
	uint32_t i_size;
	uint32_t i_atime;
	uint32_t i_ctime;
	uint32_t i_mtime;
	uint32_t i_dtime;
	uint16_t i_gid;
	uint16_t i_links_count;
	uint32_t i_blocks;
	uint32_t i_flags;
	uint32_t i_osd1;
	uint32_t i_block[15];  /* Direct and indirect block pointers */
	uint32_t i_generation;
	uint32_t i_file_acl;
	uint32_t i_dir_acl;
	uint32_t i_faddr;
	uint8_t i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

/* Block group descriptor */
typedef struct ext2_group_desc {
	uint32_t bg_block_bitmap;
	uint32_t bg_inode_bitmap;
	uint32_t bg_inode_table;
	uint16_t bg_free_blocks_count;
	uint16_t bg_free_inodes_count;
	uint16_t bg_used_dirs_count;
	uint16_t bg_pad;
	uint32_t bg_reserved[3];
} __attribute__((packed)) ext2_group_desc_t;

/* Context structure for ext2 import */
typedef struct ext2_import_ctx {
	int fd;				/* File descriptor for ext2 image */
	ext2_superblock_t sb;		/* Superblock */
	uint32_t block_size;		/* Block size in bytes */
	uint32_t inode_size;		/* Inode size in bytes */
	uint32_t inodes_per_group;	/* Inodes per block group */
	uint32_t blocks_per_group;	/* Blocks per block group */
	uint32_t group_count;		/* Number of block groups */
	ext2_group_desc_t *group_desc;	/* Block group descriptors */
	uint64_t total_size;		/* Total size of stream */
	uint64_t next_object_id;	/* Next object ID for ZFS objects */
	boolean_t verbose;		/* Verbose output */
	char *dataset_name;		/* Target dataset name */
} ext2_import_ctx_t;

/*
 * Helper function to read data from the ext2 image
 */
static int
ext2_read_data(ext2_import_ctx_t *ctx, off_t offset, void *buf, size_t size)
{
	if (lseek(ctx->fd, offset, SEEK_SET) == -1) {
		fprintf(stderr, "Failed to seek to offset %ld: %s\n",
		    offset, strerror(errno));
		return (-1);
	}

	ssize_t bytes_read = read(ctx->fd, buf, size);
	if (bytes_read != (ssize_t)size) {
		fprintf(stderr, "Failed to read %zu bytes: %s\n",
		    size, strerror(errno));
		return (-1);
	}

	return (0);
}

/*
 * Read and validate the ext2 superblock
 */
static int
ext2_read_superblock(ext2_import_ctx_t *ctx)
{
	/* Superblock is located at offset 1024 */
	if (ext2_read_data(ctx, 1024, &ctx->sb, sizeof (ctx->sb)) != 0) {
		return (-1);
	}

	/* Convert from little endian if needed */
	ctx->sb.s_magic = le16toh(ctx->sb.s_magic);
	ctx->sb.s_log_block_size = le32toh(ctx->sb.s_log_block_size);
	ctx->sb.s_inodes_count = le32toh(ctx->sb.s_inodes_count);
	ctx->sb.s_blocks_count = le32toh(ctx->sb.s_blocks_count);
	ctx->sb.s_inodes_per_group = le32toh(ctx->sb.s_inodes_per_group);
	ctx->sb.s_blocks_per_group = le32toh(ctx->sb.s_blocks_per_group);
	ctx->sb.s_rev_level = le32toh(ctx->sb.s_rev_level);
	ctx->sb.s_inode_size = le16toh(ctx->sb.s_inode_size);

	/* Validate magic number */
	if (ctx->sb.s_magic != EXT2_SUPER_MAGIC) {
		fprintf(stderr, "Invalid ext2 magic number: 0x%x\n",
		    ctx->sb.s_magic);
		return (-1);
	}

	/* Calculate block size */
	ctx->block_size = EXT2_BLOCK_SIZE_BASE << ctx->sb.s_log_block_size;
	if (ctx->block_size < EXT2_MIN_BLOCK_SIZE ||
	    ctx->block_size > EXT2_MAX_BLOCK_SIZE) {
		fprintf(stderr, "Invalid block size: %u\n", ctx->block_size);
		return (-1);
	}

	/* Set other derived values */
	/* Use inode size from superblock if available, otherwise use default */
	if (ctx->sb.s_rev_level >= 1) {
		ctx->inode_size = le16toh(ctx->sb.s_inode_size);
	} else {
		ctx->inode_size = 128;  /* EXT2_GOOD_OLD_INODE_SIZE */
	}
	
	ctx->inodes_per_group = ctx->sb.s_inodes_per_group;
	ctx->blocks_per_group = ctx->sb.s_blocks_per_group;
	ctx->group_count = (ctx->sb.s_blocks_count + ctx->blocks_per_group - 1) /
	    ctx->blocks_per_group;
	ctx->next_object_id = 1;  /* Start object IDs from 1 */

	if (ctx->verbose) {
		printf("EXT2 Filesystem Information:\n");
		printf("  Block size: %u bytes\n", ctx->block_size);
		printf("  Inode size: %u bytes\n", ctx->inode_size);
		printf("  Total inodes: %u\n", ctx->sb.s_inodes_count);
		printf("  Total blocks: %u\n", ctx->sb.s_blocks_count);
		printf("  Inodes per group: %u\n", ctx->inodes_per_group);
		printf("  Blocks per group: %u\n", ctx->blocks_per_group);
		printf("  Block groups: %u\n", ctx->group_count);
		printf("  Revision level: %u\n", ctx->sb.s_rev_level);
	}

	return (0);
}

/*
 * Read block group descriptors
 */
static int
ext2_read_group_descriptors(ext2_import_ctx_t *ctx)
{
	size_t desc_size = sizeof (ext2_group_desc_t) * ctx->group_count;
	off_t desc_offset;

	/* Group descriptors start in the block after the superblock */
	if (ctx->block_size == 1024) {
		desc_offset = 2048;  /* Skip boot block and superblock */
	} else {
		desc_offset = ctx->block_size;  /* Skip superblock */
	}

	if (ctx->verbose) {
		printf("Reading %u group descriptors from offset %ld\n",
		    ctx->group_count, desc_offset);
	}

	ctx->group_desc = safe_malloc(desc_size);
	if (ext2_read_data(ctx, desc_offset, ctx->group_desc, desc_size) != 0) {
		free(ctx->group_desc);
		ctx->group_desc = NULL;
		return (-1);
	}

	/* Convert from little endian if needed */
	for (uint32_t i = 0; i < ctx->group_count; i++) {
		ctx->group_desc[i].bg_block_bitmap =
		    le32toh(ctx->group_desc[i].bg_block_bitmap);
		ctx->group_desc[i].bg_inode_bitmap =
		    le32toh(ctx->group_desc[i].bg_inode_bitmap);
		ctx->group_desc[i].bg_inode_table =
		    le32toh(ctx->group_desc[i].bg_inode_table);

		if (ctx->verbose) {
			printf("Group %u: inode_table at block %u\n",
			    i, ctx->group_desc[i].bg_inode_table);
		}
	}

	return (0);
}

/*
 * Read an inode from the filesystem
 */
static int
ext2_read_inode(ext2_import_ctx_t *ctx, uint32_t inode_num, ext2_inode_t *inode)
{
	uint32_t group = (inode_num - 1) / ctx->inodes_per_group;
	uint32_t index = (inode_num - 1) % ctx->inodes_per_group;
	off_t inode_offset;

	if (group >= ctx->group_count) {
		fprintf(stderr, "Invalid inode number: %u\n", inode_num);
		return (-1);
	}

	inode_offset = (off_t)ctx->group_desc[group].bg_inode_table *
	    ctx->block_size + index * ctx->inode_size;

	if (ctx->verbose) {
		printf("Reading inode %u: group=%u, index=%u, offset=%ld\n",
		    inode_num, group, index, inode_offset);
	}

	if (ext2_read_data(ctx, inode_offset, inode, sizeof (*inode)) != 0) {
		return (-1);
	}

	/* Convert from little endian */
	inode->i_mode = le16toh(inode->i_mode);
	inode->i_uid = le16toh(inode->i_uid);
	inode->i_size = le32toh(inode->i_size);
	inode->i_atime = le32toh(inode->i_atime);
	inode->i_ctime = le32toh(inode->i_ctime);
	inode->i_mtime = le32toh(inode->i_mtime);
	inode->i_gid = le16toh(inode->i_gid);
	inode->i_links_count = le16toh(inode->i_links_count);
	inode->i_blocks = le32toh(inode->i_blocks);

	for (int i = 0; i < 15; i++) {
		inode->i_block[i] = le32toh(inode->i_block[i]);
	}

	return (0);
}

/*
 * Write a ZFS object record
 */
static int
write_object_record(ext2_import_ctx_t *ctx, uint64_t object_id,
    dmu_object_type_t obj_type, uint32_t blksz)
{
	dmu_replay_record_t drr = { 0 };
	struct drr_object *drro = &drr.drr_u.drr_object;

	drr.drr_type = DRR_OBJECT;
	drr.drr_payloadlen = 0;

	drro->drr_object = object_id;
	drro->drr_type = obj_type;
	drro->drr_bonustype = DMU_OT_SA;
	drro->drr_blksz = blksz;
	drro->drr_bonuslen = 0;
	drro->drr_checksumtype = ZIO_CHECKSUM_INHERIT;
	drro->drr_compress = ZIO_COMPRESS_INHERIT;
	drro->drr_toguid = 0;

	/* Write the record */
	if (fwrite(&drr, sizeof (drr), 1, stdout) != 1) {
		fprintf(stderr, "Failed to write object record\n");
		return (-1);
	}

	return (0);
}

/*
 * Write ZFS stream header
 */
static int
write_stream_begin(ext2_import_ctx_t *ctx)
{
	dmu_replay_record_t drr = { 0 };
	struct drr_begin *drrb = &drr.drr_u.drr_begin;

	drr.drr_type = DRR_BEGIN;
	drr.drr_payloadlen = 0;

	/* Fill in begin record */
	drrb->drr_magic = DMU_BACKUP_MAGIC;
	drrb->drr_versioninfo = DMU_SUBSTREAM;
	drrb->drr_creation_time = time(NULL);
	drrb->drr_type = DMU_OST_ZFS;
	strlcpy(drrb->drr_toname, ctx->dataset_name, sizeof (drrb->drr_toname));
	drrb->drr_toguid = 0;  /* Will be assigned by receiving system */

	/* Write the record */
	if (fwrite(&drr, sizeof (drr), 1, stdout) != 1) {
		fprintf(stderr, "Failed to write stream begin record\n");
		return (-1);
	}

	return (0);
}

/*
 * Write ZFS stream end record
 */
static int
write_stream_end(ext2_import_ctx_t *ctx)
{
	(void) ctx;  /* Suppress unused parameter warning */
	dmu_replay_record_t drr = { 0 };

	drr.drr_type = DRR_END;
	drr.drr_payloadlen = 0;
	drr.drr_u.drr_end.drr_toguid = 0;

	/* Write the record */
	if (fwrite(&drr, sizeof (drr), 1, stdout) != 1) {
		fprintf(stderr, "Failed to write stream end record\n");
		return (-1);
	}

	return (0);
}

/*
 * Process the ext2 filesystem and generate ZFS stream
 */
static int
process_ext2_filesystem(ext2_import_ctx_t *ctx)
{
	ext2_inode_t root_inode;

	if (ctx->verbose) {
		printf("Processing ext2 filesystem...\n");
	}

	/* Read block group descriptors */
	if (ext2_read_group_descriptors(ctx) != 0) {
		return (-1);
	}

	/* Write stream header */
	if (write_stream_begin(ctx) != 0) {
		return (-1);
	}

	/* Read the root directory inode */
	if (ext2_read_inode(ctx, EXT2_ROOT_INODE, &root_inode) != 0) {
		fprintf(stderr, "Failed to read root directory inode\n");
		return (-1);
	}

	if (ctx->verbose) {
		printf("Root directory inode:\n");
		printf("  Mode: 0%o\n", root_inode.i_mode);
		printf("  Size: %u bytes\n", root_inode.i_size);
		printf("  Links: %u\n", root_inode.i_links_count);
	}

	/* Create root directory object in ZFS */
	if (write_object_record(ctx, ctx->next_object_id++, DMU_OT_DIRECTORY_CONTENTS,
	    SPA_OLD_MAXBLOCKSIZE) != 0) {
		return (-1);
	}

	if (ctx->verbose) {
		printf("Created ZFS directory object for root\n");
	}

	/* Write stream trailer */
	if (write_stream_end(ctx) != 0) {
		return (-1);
	}

	if (ctx->verbose) {
		printf("Successfully generated ZFS stream\n");
	}

	return (0);
}

/*
 * Main ext2 import function
 */
int
zstream_do_ext2_import(int argc, char *argv[])
{
	int c;
	boolean_t verbose = B_FALSE;
	char *ext2_image = NULL;
	char *dataset_name = NULL;
	ext2_import_ctx_t ctx = { 0 };

	/* Parse command line options */
	while ((c = getopt(argc, argv, "v")) != -1) {
		switch (c) {
		case 'v':
			verbose = B_TRUE;
			break;
		default:
			fprintf(stderr, "Invalid option\n");
			zstream_usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 2) {
		fprintf(stderr, "ext2-import requires EXT2_IMAGE_FILE and "
		    "DATASET_NAME arguments\n");
		zstream_usage();
		return (1);
	}

	ext2_image = argv[0];
	dataset_name = argv[1];

	/* Initialize context */
	ctx.verbose = verbose;
	ctx.dataset_name = dataset_name;

	/* Open ext2 image file */
	ctx.fd = open(ext2_image, O_RDONLY);
	if (ctx.fd == -1) {
		fprintf(stderr, "Failed to open %s: %s\n",
		    ext2_image, strerror(errno));
		return (1);
	}

	/* Read and validate superblock */
	if (ext2_read_superblock(&ctx) != 0) {
		close(ctx.fd);
		return (1);
	}

	/* Process the filesystem and generate stream */
	int ret = process_ext2_filesystem(&ctx);

	/* Cleanup */
	if (ctx.group_desc != NULL) {
		free(ctx.group_desc);
	}
	close(ctx.fd);
	return (ret);
}