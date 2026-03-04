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
 * CDDL HEADER END
 */

#ifndef	_SYS_BCLONE_DEDUP_H
#define	_SYS_BCLONE_DEDUP_H

#include <sys/avl.h>
#include <sys/zio.h>
#include <sys/spa.h>
#include <sys/dmu.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct dsl_dataset;

typedef struct bdi_entry {
	zio_cksum_t	bdie_cksum;		/* checksum value */
	uint8_t		bdie_cksum_type;	/* checksum algorithm */
	uint8_t		bdie_compress;		/* compression algorithm */
	uint8_t		bdie_deferred;		/* BP not yet resolved */
	uint8_t		bdie_pad;
	uint32_t	bdie_lsize;		/* logical size */
	uint32_t	bdie_psize;		/* physical size */
	blkptr_t	bdie_bp;		/* full BP for clone ref */
	uint64_t	bdie_object;		/* obj (for deferred lookup) */
	uint64_t	bdie_offset;		/* off (for deferred lookup) */
	avl_node_t	bdie_avl_link;
} bdi_entry_t;

typedef struct bclone_dedup_index {
	avl_tree_t	bdi_tree;
	uint64_t	bdi_count;		/* number of entries */
	uint64_t	bdi_mem_used;		/* current memory usage */
	uint64_t	bdi_mem_max;		/* cap from tunable */
	/* stats */
	uint64_t	bdi_hits;
	uint64_t	bdi_misses;
	uint64_t	bdi_deferred_misses;	/* deferred entry, txg not synced */
	uint64_t	bdi_cloned_bytes;
} bclone_dedup_index_t;

bclone_dedup_index_t *bdi_create(uint64_t mem_max);
void bdi_destroy(bclone_dedup_index_t *bdi);
int bdi_populate_from_dataset(bclone_dedup_index_t *bdi,
    struct dsl_dataset *ds);
bdi_entry_t *bdi_lookup_entry(bclone_dedup_index_t *bdi,
    const zio_cksum_t *cksum, uint8_t cksum_type,
    uint8_t compress, uint32_t lsize, uint32_t psize);
void bdi_insert_deferred(bclone_dedup_index_t *bdi,
    const zio_cksum_t *cksum, uint8_t cksum_type,
    uint8_t compress, uint32_t lsize, uint32_t psize,
    uint64_t object, uint64_t offset);

extern uint64_t zfs_recv_bclone_dedup_max_bytes;

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_BCLONE_DEDUP_H */
