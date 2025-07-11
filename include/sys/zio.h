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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2011 Nexenta Systems, Inc. All rights reserved.
 * Copyright (c) 2012, 2024 by Delphix. All rights reserved.
 * Copyright (c) 2013 by Saso Kiselkov. All rights reserved.
 * Copyright (c) 2013, Joyent, Inc. All rights reserved.
 * Copyright 2016 Toomas Soome <tsoome@me.com>
 * Copyright (c) 2019, Allan Jude
 * Copyright (c) 2019, 2023, 2024, Klara Inc.
 * Copyright (c) 2019-2020, Michael Niewöhner
 * Copyright (c) 2024 by George Melikov. All rights reserved.
 */

#ifndef _ZIO_H
#define	_ZIO_H

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/avl.h>
#include <sys/fs/zfs.h>
#include <sys/zio_impl.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Embedded checksum
 */
#define	ZEC_MAGIC	0x210da7ab10c7a11ULL

typedef struct zio_eck {
	uint64_t	zec_magic;	/* for validation, endianness	*/
	zio_cksum_t	zec_cksum;	/* 256-bit checksum		*/
} zio_eck_t;

/*
 * Gang block headers are self-checksumming and contain an array
 * of block pointers.
 */
#define	SPA_GANGBLOCKSIZE	SPA_MINBLOCKSIZE
#define	SPA_GBH_NBLKPTRS	((SPA_GANGBLOCKSIZE - \
	sizeof (zio_eck_t)) / sizeof (blkptr_t))
#define	SPA_GBH_FILLER		((SPA_GANGBLOCKSIZE - \
	sizeof (zio_eck_t) - \
	(SPA_GBH_NBLKPTRS * sizeof (blkptr_t))) /\
	sizeof (uint64_t))

typedef struct zio_gbh {
	blkptr_t		zg_blkptr[SPA_GBH_NBLKPTRS];
	uint64_t		zg_filler[SPA_GBH_FILLER];
	zio_eck_t		zg_tail;
} zio_gbh_phys_t;

enum zio_checksum {
	ZIO_CHECKSUM_INHERIT = 0,
	ZIO_CHECKSUM_ON,
	ZIO_CHECKSUM_OFF,
	ZIO_CHECKSUM_LABEL,
	ZIO_CHECKSUM_GANG_HEADER,
	ZIO_CHECKSUM_ZILOG,
	ZIO_CHECKSUM_FLETCHER_2,
	ZIO_CHECKSUM_FLETCHER_4,
	ZIO_CHECKSUM_SHA256,
	ZIO_CHECKSUM_ZILOG2,
	ZIO_CHECKSUM_NOPARITY,
	ZIO_CHECKSUM_SHA512,
	ZIO_CHECKSUM_SKEIN,
	ZIO_CHECKSUM_EDONR,
	ZIO_CHECKSUM_BLAKE3,
	ZIO_CHECKSUM_FUNCTIONS
};

/*
 * The number of "legacy" compression functions which can be set on individual
 * objects.
 */
#define	ZIO_CHECKSUM_LEGACY_FUNCTIONS ZIO_CHECKSUM_ZILOG2

#define	ZIO_CHECKSUM_ON_VALUE	ZIO_CHECKSUM_FLETCHER_4
#define	ZIO_CHECKSUM_DEFAULT	ZIO_CHECKSUM_ON

#define	ZIO_CHECKSUM_MASK	0xffULL
#define	ZIO_CHECKSUM_VERIFY	(1U << 8)

#define	ZIO_DEDUPCHECKSUM	ZIO_CHECKSUM_SHA256

/* macros defining encryption lengths */
#define	ZIO_OBJSET_MAC_LEN		32
#define	ZIO_DATA_IV_LEN			12
#define	ZIO_DATA_SALT_LEN		8
#define	ZIO_DATA_MAC_LEN		16

/*
 * The number of "legacy" compression functions which can be set on individual
 * objects.
 */
#define	ZIO_COMPRESS_LEGACY_FUNCTIONS ZIO_COMPRESS_LZ4

/*
 * The meaning of "compress = on" selected by the compression features enabled
 * on a given pool.
 */
#define	ZIO_COMPRESS_LEGACY_ON_VALUE	ZIO_COMPRESS_LZJB
#define	ZIO_COMPRESS_LZ4_ON_VALUE	ZIO_COMPRESS_LZ4

#define	ZIO_COMPRESS_DEFAULT		ZIO_COMPRESS_ON

#define	BOOTFS_COMPRESS_VALID(compress)			\
	((compress) == ZIO_COMPRESS_LZJB ||		\
	(compress) == ZIO_COMPRESS_LZ4 ||		\
	(compress) == ZIO_COMPRESS_GZIP_1 ||		\
	(compress) == ZIO_COMPRESS_GZIP_2 ||		\
	(compress) == ZIO_COMPRESS_GZIP_3 ||		\
	(compress) == ZIO_COMPRESS_GZIP_4 ||		\
	(compress) == ZIO_COMPRESS_GZIP_5 ||		\
	(compress) == ZIO_COMPRESS_GZIP_6 ||		\
	(compress) == ZIO_COMPRESS_GZIP_7 ||		\
	(compress) == ZIO_COMPRESS_GZIP_8 ||		\
	(compress) == ZIO_COMPRESS_GZIP_9 ||		\
	(compress) == ZIO_COMPRESS_ZLE ||		\
	(compress) == ZIO_COMPRESS_ZSTD ||		\
	(compress) == ZIO_COMPRESS_ON ||		\
	(compress) == ZIO_COMPRESS_OFF)


#define	ZIO_COMPRESS_ALGO(x)	(x & SPA_COMPRESSMASK)
#define	ZIO_COMPRESS_LEVEL(x)	((x & ~SPA_COMPRESSMASK) >> SPA_COMPRESSBITS)
#define	ZIO_COMPRESS_RAW(type, level)	(type | ((level) << SPA_COMPRESSBITS))

#define	ZIO_COMPLEVEL_ZSTD(level)	\
	ZIO_COMPRESS_RAW(ZIO_COMPRESS_ZSTD, level)

#define	ZIO_FAILURE_MODE_WAIT		0
#define	ZIO_FAILURE_MODE_CONTINUE	1
#define	ZIO_FAILURE_MODE_PANIC		2

typedef enum zio_suspend_reason {
	ZIO_SUSPEND_NONE = 0,
	ZIO_SUSPEND_IOERR,
	ZIO_SUSPEND_MMP,
} zio_suspend_reason_t;

/*
 * This was originally an enum type. However, those are 32-bit and there is no
 * way to make a 64-bit enum type. Since we ran out of bits for flags, we were
 * forced to upgrade it to a uint64_t.
 *
 * NOTE: PLEASE UPDATE THE BITFIELD STRINGS IN zfs_valstr.c IF YOU ADD ANOTHER
 * FLAG.
 */
typedef uint64_t zio_flag_t;
	/*
	 * Flags inherited by gang, ddt, and vdev children,
	 * and that must be equal for two zios to aggregate
	 */
#define	ZIO_FLAG_DONT_AGGREGATE	(1ULL << 0)
#define	ZIO_FLAG_IO_REPAIR	(1ULL << 1)
#define	ZIO_FLAG_SELF_HEAL	(1ULL << 2)
#define	ZIO_FLAG_RESILVER	(1ULL << 3)
#define	ZIO_FLAG_SCRUB		(1ULL << 4)
#define	ZIO_FLAG_SCAN_THREAD	(1ULL << 5)
#define	ZIO_FLAG_PHYSICAL	(1ULL << 6)

#define	ZIO_FLAG_AGG_INHERIT	(ZIO_FLAG_CANFAIL - 1)

	/*
	 * Flags inherited by ddt, gang, and vdev children.
	 */
#define	ZIO_FLAG_CANFAIL	(1ULL << 7)	/* must be first for INHERIT */
#define	ZIO_FLAG_SPECULATIVE	(1ULL << 8)
#define	ZIO_FLAG_CONFIG_WRITER	(1ULL << 9)
#define	ZIO_FLAG_DONT_RETRY	(1ULL << 10)
#define	ZIO_FLAG_NODATA		(1ULL << 12)
#define	ZIO_FLAG_INDUCE_DAMAGE	(1ULL << 13)
#define	ZIO_FLAG_ALLOC_THROTTLED	(1ULL << 14)

#define	ZIO_FLAG_DDT_INHERIT	(ZIO_FLAG_IO_RETRY - 1)
#define	ZIO_FLAG_GANG_INHERIT	(ZIO_FLAG_IO_RETRY - 1)

	/*
	 * Flags inherited by vdev children.
	 */
#define	ZIO_FLAG_IO_RETRY	(1ULL << 15)	/* must be first for INHERIT */
#define	ZIO_FLAG_PROBE		(1ULL << 16)
#define	ZIO_FLAG_TRYHARD	(1ULL << 17)
#define	ZIO_FLAG_OPTIONAL	(1ULL << 18)
#define	ZIO_FLAG_DIO_READ	(1ULL << 19)
#define	ZIO_FLAG_VDEV_INHERIT	(ZIO_FLAG_DONT_QUEUE - 1)

	/*
	 * Flags not inherited by any children.
	 */
#define	ZIO_FLAG_DONT_QUEUE	(1ULL << 20)	/* must be first for INHERIT */
#define	ZIO_FLAG_DONT_PROPAGATE	(1ULL << 21)
#define	ZIO_FLAG_IO_BYPASS	(1ULL << 22)
#define	ZIO_FLAG_IO_REWRITE	(1ULL << 23)
#define	ZIO_FLAG_RAW_COMPRESS	(1ULL << 24)
#define	ZIO_FLAG_RAW_ENCRYPT	(1ULL << 25)
#define	ZIO_FLAG_GANG_CHILD	(1ULL << 26)
#define	ZIO_FLAG_DDT_CHILD	(1ULL << 27)
#define	ZIO_FLAG_GODFATHER	(1ULL << 28)
#define	ZIO_FLAG_NOPWRITE	(1ULL << 29)
#define	ZIO_FLAG_REEXECUTED	(1ULL << 30)
#define	ZIO_FLAG_DELEGATED	(1ULL << 31)
#define	ZIO_FLAG_PREALLOCATED	(1ULL << 32)

#define	ZIO_ALLOCATOR_NONE	(-1)
#define	ZIO_HAS_ALLOCATOR(zio)	((zio)->io_allocator != ZIO_ALLOCATOR_NONE)

#define	ZIO_FLAG_MUSTSUCCEED		0
#define	ZIO_FLAG_RAW	(ZIO_FLAG_RAW_COMPRESS | ZIO_FLAG_RAW_ENCRYPT)

#define	ZIO_DDT_CHILD_FLAGS(zio)				\
	(((zio)->io_flags & ZIO_FLAG_DDT_INHERIT) |		\
	ZIO_FLAG_DDT_CHILD | ZIO_FLAG_CANFAIL)

#define	ZIO_GANG_CHILD_FLAGS(zio)				\
	(((zio)->io_flags & ZIO_FLAG_GANG_INHERIT) |		\
	ZIO_FLAG_GANG_CHILD | ZIO_FLAG_CANFAIL)

#define	ZIO_VDEV_CHILD_FLAGS(zio)				\
	(((zio)->io_flags & ZIO_FLAG_VDEV_INHERIT) |		\
	ZIO_FLAG_DONT_PROPAGATE | ZIO_FLAG_CANFAIL)

#define	ZIO_CHILD_BIT(x)		(1U << (x))
#define	ZIO_CHILD_BIT_IS_SET(val, x)	((val) & (1U << (x)))


/*
 * ZIOs that are ZIO_FLAG_IMPORTANT are always queued so that they never get
 * starved out. This allows us to bypass the queue for "normal" reads and
 * writes when the queues are low for better IOPS. If the queues get too high
 * then we go back to queuing the "normal" reads/writes so as not to starve
 * out more important IOs like scrub/resilver/retry. See
 * zfs_vdev_queue_bypass_pct for details.
 */

#define	ZIO_FLAG_IMPORTANT					\
	ZIO_FLAG_IO_REPAIR | ZIO_FLAG_SELF_HEAL |		\
	ZIO_FLAG_RESILVER | ZIO_FLAG_SCRUB |			\
	ZIO_FLAG_IO_RETRY | ZIO_FLAG_NODATA

#define	ZIO_IS_NORMAL(zio)					\
	!((zio)->io_flags & (ZIO_FLAG_IMPORTANT))

enum zio_child {
	ZIO_CHILD_VDEV = 0,
	ZIO_CHILD_GANG,
	ZIO_CHILD_DDT,
	ZIO_CHILD_LOGICAL,
	ZIO_CHILD_TYPES
};

#define	ZIO_CHILD_VDEV_BIT		ZIO_CHILD_BIT(ZIO_CHILD_VDEV)
#define	ZIO_CHILD_GANG_BIT		ZIO_CHILD_BIT(ZIO_CHILD_GANG)
#define	ZIO_CHILD_DDT_BIT		ZIO_CHILD_BIT(ZIO_CHILD_DDT)
#define	ZIO_CHILD_LOGICAL_BIT		ZIO_CHILD_BIT(ZIO_CHILD_LOGICAL)
#define	ZIO_CHILD_ALL_BITS					\
	(ZIO_CHILD_VDEV_BIT | ZIO_CHILD_GANG_BIT |		\
	ZIO_CHILD_DDT_BIT | ZIO_CHILD_LOGICAL_BIT)

enum zio_wait_type {
	ZIO_WAIT_READY = 0,
	ZIO_WAIT_DONE,
	ZIO_WAIT_TYPES
};

typedef void zio_done_func_t(zio_t *zio);

extern int zio_exclude_metadata;
extern int zio_dva_throttle_enabled;
extern const char *const zio_type_name[ZIO_TYPES];

/*
 * A bookmark is a four-tuple <objset, object, level, blkid> that uniquely
 * identifies any block in the pool.  By convention, the meta-objset (MOS)
 * is objset 0, and the meta-dnode is object 0.  This covers all blocks
 * except root blocks and ZIL blocks, which are defined as follows:
 *
 * Root blocks (objset_phys_t) are object 0, level -1:  <objset, 0, -1, 0>.
 * ZIL blocks are bookmarked <objset, 0, -2, blkid == ZIL sequence number>.
 * dmu_sync()ed ZIL data blocks are bookmarked <objset, object, -2, blkid>.
 * dnode visit bookmarks are <objset, object id of dnode, -3, 0>.
 *
 * Note: this structure is called a bookmark because its original purpose
 * was to remember where to resume a pool-wide traverse.
 *
 * Note: this structure is passed between userland and the kernel, and is
 * stored on disk (by virtue of being incorporated into other on-disk
 * structures, e.g. dsl_scan_phys_t).
 *
 * If the head_errlog feature is enabled a different on-disk format for error
 * logs is used. This introduces the use of an error bookmark, a four-tuple
 * <object, level, blkid, birth> that uniquely identifies any error block
 * in the pool. The birth transaction group is used to track whether the block
 * has been overwritten by newer data or added to a snapshot since its marking
 * as an error.
 */
struct zbookmark_phys {
	uint64_t	zb_objset;
	uint64_t	zb_object;
	int64_t		zb_level;
	uint64_t	zb_blkid;
};

struct zbookmark_err_phys {
	uint64_t	zb_object;
	int64_t		zb_level;
	uint64_t	zb_blkid;
	uint64_t	zb_birth;
};

#define	SET_BOOKMARK(zb, objset, object, level, blkid)  \
{                                                       \
	(zb)->zb_objset = objset;                       \
	(zb)->zb_object = object;                       \
	(zb)->zb_level = level;                         \
	(zb)->zb_blkid = blkid;                         \
}

#define	ZB_DESTROYED_OBJSET	(-1ULL)

#define	ZB_ROOT_OBJECT		(0ULL)
#define	ZB_ROOT_LEVEL		(-1LL)
#define	ZB_ROOT_BLKID		(0ULL)

#define	ZB_ZIL_OBJECT		(0ULL)
#define	ZB_ZIL_LEVEL		(-2LL)

#define	ZB_DNODE_LEVEL		(-3LL)
#define	ZB_DNODE_BLKID		(0ULL)

#define	ZB_IS_ZERO(zb)						\
	((zb)->zb_objset == 0 && (zb)->zb_object == 0 &&	\
	(zb)->zb_level == 0 && (zb)->zb_blkid == 0)
#define	ZB_IS_ROOT(zb)				\
	((zb)->zb_object == ZB_ROOT_OBJECT &&	\
	(zb)->zb_level == ZB_ROOT_LEVEL &&	\
	(zb)->zb_blkid == ZB_ROOT_BLKID)

typedef struct zio_prop {
	enum zio_checksum	zp_checksum;
	enum zio_compress	zp_compress;
	uint8_t			zp_complevel;
	uint8_t			zp_level;
	uint8_t			zp_copies;
	uint8_t			zp_gang_copies;
	dmu_object_type_t	zp_type;
	boolean_t		zp_dedup;
	boolean_t		zp_dedup_verify;
	boolean_t		zp_nopwrite;
	boolean_t		zp_brtwrite;
	boolean_t		zp_encrypt;
	boolean_t		zp_byteorder;
	boolean_t		zp_direct_write;
	uint8_t			zp_salt[ZIO_DATA_SALT_LEN];
	uint8_t			zp_iv[ZIO_DATA_IV_LEN];
	uint8_t			zp_mac[ZIO_DATA_MAC_LEN];
	uint32_t		zp_zpl_smallblk;
	dmu_object_type_t	zp_storage_type;
} zio_prop_t;

typedef struct zio_cksum_report zio_cksum_report_t;

typedef void zio_cksum_finish_f(zio_cksum_report_t *rep,
    const abd_t *good_data);
typedef void zio_cksum_free_f(void *cbdata, size_t size);

struct zio_bad_cksum;				/* defined in zio_checksum.h */
struct dnode_phys;
struct abd;

struct zio_cksum_report {
	struct zio_cksum_report *zcr_next;
	nvlist_t		*zcr_ereport;
	nvlist_t		*zcr_detector;
	void			*zcr_cbdata;
	size_t			zcr_cbinfo;	/* passed to zcr_free() */
	uint64_t		zcr_sector;
	uint64_t		zcr_align;
	uint64_t		zcr_length;
	zio_cksum_finish_f	*zcr_finish;
	zio_cksum_free_f	*zcr_free;

	/* internal use only */
	struct zio_bad_cksum	*zcr_ckinfo;	/* information from failure */
};

typedef struct zio_vsd_ops {
	zio_done_func_t		*vsd_free;
} zio_vsd_ops_t;

typedef struct zio_gang_node {
	zio_gbh_phys_t		*gn_gbh;
	struct zio_gang_node	*gn_child[SPA_GBH_NBLKPTRS];
} zio_gang_node_t;

typedef zio_t *zio_gang_issue_func_t(zio_t *zio, blkptr_t *bp,
    zio_gang_node_t *gn, struct abd *data, uint64_t offset);

typedef void zio_transform_func_t(zio_t *zio, struct abd *data, uint64_t size);

typedef struct zio_transform {
	struct abd		*zt_orig_abd;
	uint64_t		zt_orig_size;
	uint64_t		zt_bufsize;
	zio_transform_func_t	*zt_transform;
	struct zio_transform	*zt_next;
} zio_transform_t;

typedef zio_t *zio_pipe_stage_t(zio_t *zio);

/*
 * The io_post flags describe additional actions that a parent IO should
 * consider or perform on behalf of a child. They are distinct from io_flags
 * because the child must be able to propagate them to the parent. The normal
 * io_flags are local to the zio, not protected by any lock, and not modifiable
 * by children; the reexecute flags are protected by io_lock, modifiable by
 * children, and always propagated -- even when ZIO_FLAG_DONT_PROPAGATE is set.
 */
#define	ZIO_POST_REEXECUTE	(1 << 0)
#define	ZIO_POST_SUSPEND	(1 << 1)
#define	ZIO_POST_DIO_CHKSUM_ERR	(1 << 2)

/*
 * The io_trim flags are used to specify the type of TRIM to perform.  They
 * only apply to ZIO_TYPE_TRIM zios are distinct from io_flags.
 */
enum trim_flag {
	ZIO_TRIM_SECURE		= 1U << 0,
};

typedef struct zio_alloc_list {
	list_t  zal_list;
	uint64_t zal_size;
} zio_alloc_list_t;

typedef struct zio_link {
	zio_t		*zl_parent;
	zio_t		*zl_child;
	list_node_t	zl_parent_node;
	list_node_t	zl_child_node;
} zio_link_t;

enum zio_qstate {
	ZIO_QS_NONE = 0,
	ZIO_QS_QUEUED,
	ZIO_QS_ACTIVE,
	ZIO_QS_BYPASS,
};

struct zio {
	/* Core information about this I/O */
	zbookmark_phys_t	io_bookmark;
	zio_prop_t	io_prop;
	zio_type_t	io_type;
	enum zio_child	io_child_type;
	enum trim_flag	io_trim_flags;
	zio_priority_t	io_priority;
	uint8_t		io_post;
	uint8_t		io_state[ZIO_WAIT_TYPES];
	uint64_t	io_txg;
	spa_t		*io_spa;
	blkptr_t	*io_bp;
	blkptr_t	*io_bp_override;
	blkptr_t	io_bp_copy;
	list_t		io_parent_list;
	list_t		io_child_list;
	zio_t		*io_logical;
	zio_transform_t *io_transform_stack;

	/* Callback info */
	zio_done_func_t	*io_ready;
	zio_done_func_t	*io_children_ready;
	zio_done_func_t	*io_done;
	void		*io_private;
	int64_t		io_prev_space_delta;	/* DMU private */
	blkptr_t	io_bp_orig;
	/* io_lsize != io_orig_size iff this is a raw write */
	uint64_t	io_lsize;

	/* Data represented by this I/O */
	struct abd	*io_abd;
	struct abd	*io_orig_abd;
	uint64_t	io_size;
	uint64_t	io_orig_size;

	/* Stuff for the vdev stack */
	vdev_t		*io_vd;
	void		*io_vsd;
	const zio_vsd_ops_t *io_vsd_ops;
	metaslab_class_t *io_metaslab_class;	/* dva throttle class */

	enum zio_qstate	io_queue_state;	/* vdev queue state */
	union {
		list_node_t l;
		avl_node_t a;
	} io_queue_node ____cacheline_aligned;	/* allocator and vdev queues */
	avl_node_t	io_offset_node;	/* vdev offset queues */
	uint64_t	io_offset;
	hrtime_t	io_timestamp;	/* submitted at */
	hrtime_t	io_queued_timestamp;
	hrtime_t	io_target_timestamp;
	hrtime_t	io_delta;	/* vdev queue service delta */
	hrtime_t	io_delay;	/* Device access time (disk or */
					/* file). */
	zio_alloc_list_t 	io_alloc_list;

	/* Internal pipeline state */
	zio_flag_t	io_flags;
	enum zio_stage	io_stage;
	enum zio_stage	io_pipeline;
	zio_flag_t	io_orig_flags;
	enum zio_stage	io_orig_stage;
	enum zio_stage	io_orig_pipeline;
	enum zio_stage	io_pipeline_trace;
	int		io_error;
	int		io_child_error[ZIO_CHILD_TYPES];
	uint64_t	io_children[ZIO_CHILD_TYPES][ZIO_WAIT_TYPES];
	uint64_t	*io_stall;
	zio_t		*io_gang_leader;
	zio_gang_node_t	*io_gang_tree;
	void		*io_executor;
	void		*io_waiter;
	void		*io_bio;
	kmutex_t	io_lock;
	kcondvar_t	io_cv;
	int		io_allocator;

	/* FMA state */
	zio_cksum_report_t *io_cksum_report;
	uint64_t	io_ena;

	/* Taskq dispatching state */
	taskq_ent_t	io_tqent;
};

enum blk_verify_flag {
	BLK_VERIFY_ONLY,
	BLK_VERIFY_LOG,
	BLK_VERIFY_HALT
};

enum blk_config_flag {
	BLK_CONFIG_HELD,   // SCL_VDEV held for writer
	BLK_CONFIG_NEEDED, // SCL_VDEV should be obtained for reader
	BLK_CONFIG_NEEDED_TRY, // Try with SCL_VDEV for reader
	BLK_CONFIG_SKIP,   // skip checks which require SCL_VDEV
};

extern int zio_bookmark_compare(const void *, const void *);

extern zio_t *zio_null(zio_t *pio, spa_t *spa, vdev_t *vd,
    zio_done_func_t *done, void *priv, zio_flag_t flags);

extern zio_t *zio_root(spa_t *spa,
    zio_done_func_t *done, void *priv, zio_flag_t flags);

extern void zio_destroy(zio_t *zio);

extern zio_t *zio_read(zio_t *pio, spa_t *spa, const blkptr_t *bp,
    struct abd *data, uint64_t lsize, zio_done_func_t *done, void *priv,
    zio_priority_t priority, zio_flag_t flags, const zbookmark_phys_t *zb);

extern zio_t *zio_write(zio_t *pio, spa_t *spa, uint64_t txg, blkptr_t *bp,
    struct abd *data, uint64_t size, uint64_t psize, const zio_prop_t *zp,
    zio_done_func_t *ready, zio_done_func_t *children_ready,
    zio_done_func_t *done, void *priv, zio_priority_t priority,
    zio_flag_t flags, const zbookmark_phys_t *zb);

extern zio_t *zio_rewrite(zio_t *pio, spa_t *spa, uint64_t txg, blkptr_t *bp,
    struct abd *data, uint64_t size, zio_done_func_t *done, void *priv,
    zio_priority_t priority, zio_flag_t flags, zbookmark_phys_t *zb);

extern void zio_write_override(zio_t *zio, blkptr_t *bp, int copies,
    int gang_copies, boolean_t nopwrite, boolean_t brtwrite);

extern void zio_free(spa_t *spa, uint64_t txg, const blkptr_t *bp);

extern zio_t *zio_claim(zio_t *pio, spa_t *spa, uint64_t txg,
    const blkptr_t *bp,
    zio_done_func_t *done, void *priv, zio_flag_t flags);

extern zio_t *zio_trim(zio_t *pio, vdev_t *vd, uint64_t offset, uint64_t size,
    zio_done_func_t *done, void *priv, zio_priority_t priority,
    zio_flag_t flags, enum trim_flag trim_flags);

extern zio_t *zio_read_phys(zio_t *pio, vdev_t *vd, uint64_t offset,
    uint64_t size, struct abd *data, int checksum,
    zio_done_func_t *done, void *priv, zio_priority_t priority,
    zio_flag_t flags, boolean_t labels);

extern zio_t *zio_write_phys(zio_t *pio, vdev_t *vd, uint64_t offset,
    uint64_t size, struct abd *data, int checksum,
    zio_done_func_t *done, void *priv, zio_priority_t priority,
    zio_flag_t flags, boolean_t labels);

extern zio_t *zio_free_sync(zio_t *pio, spa_t *spa, uint64_t txg,
    const blkptr_t *bp, zio_flag_t flags);

extern int zio_alloc_zil(spa_t *spa, objset_t *os, uint64_t txg,
    blkptr_t *new_bp, uint64_t size, boolean_t *slog);
extern void zio_flush(zio_t *zio, vdev_t *vd);
extern void zio_shrink(zio_t *zio, uint64_t size);

extern size_t zio_get_compression_max_size(enum zio_compress compress,
    uint64_t gcd_alloc, uint64_t min_alloc, size_t s_len);
extern int zio_wait(zio_t *zio);
extern void zio_nowait(zio_t *zio);
extern void zio_execute(void *zio);
extern void zio_interrupt(void *zio);
extern void zio_delay_init(zio_t *zio);
extern void zio_delay_interrupt(zio_t *zio);
extern void zio_deadman(zio_t *zio, const char *tag);

extern zio_t *zio_walk_parents(zio_t *cio, zio_link_t **);
extern zio_t *zio_walk_children(zio_t *pio, zio_link_t **);
extern zio_t *zio_unique_parent(zio_t *cio);
extern void zio_add_child(zio_t *pio, zio_t *cio);

extern void *zio_buf_alloc(size_t size);
extern void zio_buf_free(void *buf, size_t size);
extern void *zio_data_buf_alloc(size_t size);
extern void zio_data_buf_free(void *buf, size_t size);

extern void zio_push_transform(zio_t *zio, struct abd *abd, uint64_t size,
    uint64_t bufsize, zio_transform_func_t *transform);
extern void zio_pop_transforms(zio_t *zio);

extern void zio_resubmit_stage_async(void *);

extern zio_t *zio_vdev_child_io(zio_t *zio, blkptr_t *bp, vdev_t *vd,
    uint64_t offset, struct abd *data, uint64_t size, int type,
    zio_priority_t priority, zio_flag_t flags,
    zio_done_func_t *done, void *priv);

extern zio_t *zio_vdev_delegated_io(vdev_t *vd, uint64_t offset,
    struct abd *data, uint64_t size, zio_type_t type, zio_priority_t priority,
    zio_flag_t flags, zio_done_func_t *done, void *priv);

extern void zio_vdev_io_bypass(zio_t *zio);
extern void zio_vdev_io_reissue(zio_t *zio);
extern void zio_vdev_io_redone(zio_t *zio);

extern void zio_change_priority(zio_t *pio, zio_priority_t priority);

extern void zio_checksum_verified(zio_t *zio);
extern void zio_dio_chksum_verify_error_report(zio_t *zio);
extern int zio_worst_error(int e1, int e2);

extern enum zio_checksum zio_checksum_select(enum zio_checksum child,
    enum zio_checksum parent);
extern enum zio_checksum zio_checksum_dedup_select(spa_t *spa,
    enum zio_checksum child, enum zio_checksum parent);
extern enum zio_compress zio_compress_select(spa_t *spa,
    enum zio_compress child, enum zio_compress parent);
extern uint8_t zio_complevel_select(spa_t *spa, enum zio_compress compress,
    uint8_t child, uint8_t parent);

extern void zio_suspend(spa_t *spa, zio_t *zio, zio_suspend_reason_t);
extern int zio_resume(spa_t *spa);
extern void zio_resume_wait(spa_t *spa);

extern int zfs_blkptr_verify(spa_t *spa, const blkptr_t *bp,
    enum blk_config_flag blk_config, enum blk_verify_flag blk_verify);

/*
 * Initial setup and teardown.
 */
extern void zio_init(void);
extern void zio_fini(void);

/*
 * Fault injection
 */
struct zinject_record;
extern uint32_t zio_injection_enabled;
extern int zio_inject_fault(char *name, int flags, int *id,
    struct zinject_record *record);
extern int zio_inject_list_next(int *id, char *name, size_t buflen,
    struct zinject_record *record);
extern int zio_clear_fault(int id);
extern void zio_handle_panic_injection(spa_t *spa, const char *tag,
    uint64_t type);
extern int zio_handle_decrypt_injection(spa_t *spa, const zbookmark_phys_t *zb,
    uint64_t type, int error);
extern int zio_handle_fault_injection(zio_t *zio, int error);
extern int zio_handle_device_injection(vdev_t *vd, zio_t *zio, int error);
extern int zio_handle_device_injections(vdev_t *vd, zio_t *zio, int err1,
    int err2);
extern int zio_handle_label_injection(zio_t *zio, int error);
extern void zio_handle_ignored_writes(zio_t *zio);
extern hrtime_t zio_handle_io_delay(zio_t *zio);
extern void zio_handle_import_delay(spa_t *spa, hrtime_t elapsed);
extern void zio_handle_export_delay(spa_t *spa, hrtime_t elapsed);

/*
 * Checksum ereport functions
 */
extern int zfs_ereport_start_checksum(spa_t *spa, vdev_t *vd,
    const zbookmark_phys_t *zb, struct zio *zio, uint64_t offset,
    uint64_t length, struct zio_bad_cksum *info);
extern void zfs_ereport_finish_checksum(zio_cksum_report_t *report,
    const abd_t *good_data, const abd_t *bad_data, boolean_t drop_if_identical);

extern void zfs_ereport_free_checksum(zio_cksum_report_t *report);

/* If we have the good data in hand, this function can be used */
extern int zfs_ereport_post_checksum(spa_t *spa, vdev_t *vd,
    const zbookmark_phys_t *zb, struct zio *zio, uint64_t offset,
    uint64_t length, const abd_t *good_data, const abd_t *bad_data,
    struct zio_bad_cksum *info);

void zio_vsd_default_cksum_report(zio_t *zio, zio_cksum_report_t *zcr);
extern void zfs_ereport_snapshot_post(const char *subclass, spa_t *spa,
    const char *name);

/* Called from spa_sync(), but primarily an injection handler */
extern void spa_handle_ignored_writes(spa_t *spa);

/* zbookmark_phys functions */
boolean_t zbookmark_subtree_completed(const struct dnode_phys *dnp,
    const zbookmark_phys_t *subtree_root, const zbookmark_phys_t *last_block);
boolean_t zbookmark_subtree_tbd(const struct dnode_phys *dnp,
    const zbookmark_phys_t *subtree_root, const zbookmark_phys_t *last_block);
int zbookmark_compare(uint16_t dbss1, uint8_t ibs1, uint16_t dbss2,
    uint8_t ibs2, const zbookmark_phys_t *zb1, const zbookmark_phys_t *zb2);

#ifdef	__cplusplus
}
#endif

#endif	/* _ZIO_H */
