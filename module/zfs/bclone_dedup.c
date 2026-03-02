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

/*
 * Ephemeral in-memory checksum index for receive-side block cloning
 * deduplication (zfs recv -B).
 *
 * Builds an AVL tree index of (checksum, algorithm, compression, lsize, psize)
 * → blkptr_t from an existing dataset's metadata via traverse_dataset().
 * During receive, incoming blocks are looked up in this index; matches are
 * cloned via BRT (Block Reference Table) instead of being re-written.
 */

#include <sys/bclone_dedup.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dnode.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/zio.h>

uint64_t zfs_recv_bclone_dedup_max_bytes = 1ULL << 30; /* 1 GB */

/*
 * AVL comparator: compare by (cksum_type, cksum[0..3], compress, lsize, psize).
 * Exact-match semantics.
 */
static int
bdi_entry_compare(const void *va, const void *vb)
{
	const bdi_entry_t *a = va;
	const bdi_entry_t *b = vb;

	int cmp = TREE_CMP(a->bdie_cksum_type, b->bdie_cksum_type);
	if (cmp != 0)
		return (cmp);

	for (int i = 0; i < 4; i++) {
		cmp = TREE_CMP(a->bdie_cksum.zc_word[i],
		    b->bdie_cksum.zc_word[i]);
		if (cmp != 0)
			return (cmp);
	}

	cmp = TREE_CMP(a->bdie_compress, b->bdie_compress);
	if (cmp != 0)
		return (cmp);

	cmp = TREE_CMP(a->bdie_lsize, b->bdie_lsize);
	if (cmp != 0)
		return (cmp);

	return (TREE_CMP(a->bdie_psize, b->bdie_psize));
}

bclone_dedup_index_t *
bdi_create(uint64_t mem_max)
{
	bclone_dedup_index_t *bdi;

	bdi = kmem_zalloc(sizeof (*bdi), KM_SLEEP);
	avl_create(&bdi->bdi_tree, bdi_entry_compare, sizeof (bdi_entry_t),
	    offsetof(bdi_entry_t, bdie_avl_link));
	mutex_init(&bdi->bdi_lock, NULL, MUTEX_DEFAULT, NULL);
	bdi->bdi_mem_max = mem_max;

	return (bdi);
}

void
bdi_destroy(bclone_dedup_index_t *bdi)
{
	void *cookie = NULL;
	bdi_entry_t *entry;

	while ((entry = avl_destroy_nodes(&bdi->bdi_tree, &cookie)) != NULL)
		kmem_free(entry, sizeof (*entry));

	avl_destroy(&bdi->bdi_tree);
	mutex_destroy(&bdi->bdi_lock);
	kmem_free(bdi, sizeof (*bdi));
}

/*
 * Traversal callback for bdi_populate_from_dataset().
 * Inserts each eligible block pointer into the index.
 */
static int
bdi_traverse_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	(void) spa, (void) zilog, (void) zb, (void) dnp;
	bclone_dedup_index_t *bdi = arg;

	if (bp == NULL || BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (0);

	if (BP_IS_GANG(bp))
		return (0);

	/* Respect memory cap */
	if (bdi->bdi_mem_used + sizeof (bdi_entry_t) > bdi->bdi_mem_max) {
		zfs_dbgmsg("bclone_dedup: memory cap reached at %llu entries "
		    "(%llu bytes)", (u_longlong_t)bdi->bdi_count,
		    (u_longlong_t)bdi->bdi_mem_used);
		return (0);
	}

	bdi_entry_t *entry = kmem_alloc(sizeof (*entry), KM_SLEEP);
	entry->bdie_cksum = bp->blk_cksum;
	entry->bdie_cksum_type = BP_GET_CHECKSUM(bp);
	entry->bdie_compress = BP_GET_COMPRESS(bp);
	entry->bdie_deferred = 0;
	entry->bdie_pad = 0;
	entry->bdie_lsize = BP_GET_LSIZE(bp);
	entry->bdie_psize = BP_GET_PSIZE(bp);
	entry->bdie_bp = *bp;
	entry->bdie_object = 0;
	entry->bdie_offset = 0;

	avl_index_t where;
	if (avl_find(&bdi->bdi_tree, entry, &where) != NULL) {
		/* Duplicate — keep existing entry */
		kmem_free(entry, sizeof (*entry));
	} else {
		avl_insert(&bdi->bdi_tree, entry, where);
		bdi->bdi_count++;
		bdi->bdi_mem_used += sizeof (*entry);
	}

	return (0);
}

int
bdi_populate_from_dataset(bclone_dedup_index_t *bdi, dsl_dataset_t *ds)
{
	int err;

	err = traverse_dataset(ds, 0,
	    TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA, bdi_traverse_cb, bdi);

	if (err == 0) {
		zfs_dbgmsg("bclone_dedup: indexed %llu entries (%llu bytes) "
		    "from dataset %llu",
		    (u_longlong_t)bdi->bdi_count,
		    (u_longlong_t)bdi->bdi_mem_used,
		    (u_longlong_t)ds->ds_object);
	}

	return (err);
}

/*
 * Look up a block in the index by checksum, algorithm, compression, and
 * geometry.  Returns the matching blkptr_t or NULL on miss.
 * Only returns entries with a resolved BP (non-deferred).
 */
const blkptr_t *
bdi_lookup(bclone_dedup_index_t *bdi, const zio_cksum_t *cksum,
    uint8_t cksum_type, uint8_t compress, uint32_t lsize, uint32_t psize)
{
	bdi_entry_t search = {
		.bdie_cksum = *cksum,
		.bdie_cksum_type = cksum_type,
		.bdie_compress = compress,
		.bdie_lsize = lsize,
		.bdie_psize = psize,
	};

	bdi_entry_t *found = avl_find(&bdi->bdi_tree, &search, NULL);
	if (found != NULL && !found->bdie_deferred)
		return (&found->bdie_bp);

	return (NULL);
}

/*
 * Look up a block in the index, returning the entry itself (including
 * deferred entries).  The caller must check bdie_deferred to determine
 * whether the BP is valid or needs resolution via dmu_read_l0_bps().
 */
bdi_entry_t *
bdi_lookup_entry(bclone_dedup_index_t *bdi, const zio_cksum_t *cksum,
    uint8_t cksum_type, uint8_t compress, uint32_t lsize, uint32_t psize)
{
	bdi_entry_t search = {
		.bdie_cksum = *cksum,
		.bdie_cksum_type = cksum_type,
		.bdie_compress = compress,
		.bdie_lsize = lsize,
		.bdie_psize = psize,
	};

	return (avl_find(&bdi->bdi_tree, &search, NULL));
}

/*
 * Insert a deferred entry for a block that was just written during recv.
 * The BP is not yet available (block hasn't synced); the caller records
 * (object, offset) so a future lookup can resolve the BP via
 * dmu_read_l0_bps() once the txg syncs.
 *
 * Called only from the single recv writer thread — no locking needed.
 */
void
bdi_insert_deferred(bclone_dedup_index_t *bdi, const zio_cksum_t *cksum,
    uint8_t cksum_type, uint8_t compress, uint32_t lsize, uint32_t psize,
    uint64_t object, uint64_t offset)
{
	if (bdi->bdi_mem_used + sizeof (bdi_entry_t) > bdi->bdi_mem_max)
		return;

	bdi_entry_t *entry = kmem_alloc(sizeof (*entry), KM_SLEEP);
	entry->bdie_cksum = *cksum;
	entry->bdie_cksum_type = cksum_type;
	entry->bdie_compress = compress;
	entry->bdie_deferred = 1;
	entry->bdie_pad = 0;
	entry->bdie_lsize = lsize;
	entry->bdie_psize = psize;
	BP_ZERO(&entry->bdie_bp);
	entry->bdie_object = object;
	entry->bdie_offset = offset;

	avl_index_t where;
	if (avl_find(&bdi->bdi_tree, entry, &where) != NULL) {
		/* Already in the index (pre-built or earlier deferred) */
		kmem_free(entry, sizeof (*entry));
	} else {
		avl_insert(&bdi->bdi_tree, entry, where);
		bdi->bdi_count++;
		bdi->bdi_mem_used += sizeof (*entry);
	}
}

ZFS_MODULE_PARAM(zfs_recv, zfs_recv_, bclone_dedup_max_bytes, U64, ZMOD_RW,
	"Maximum memory for bclone dedup index");
