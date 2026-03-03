#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -R -B (recursive replication with bclone dedup)
# works correctly.  Child datasets should automatically dedup against
# the parent dataset that was received first in the stream.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -R -B deduplicates child datasets against parent."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create source hierarchy with shared data between parent and children
log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/shared bs=128K count=16

log_must zfs create -o checksum=sha256 $SRCFS/child1
# Copy the SAME data to child1 — this is the cross-dataset dedup opportunity
log_must cp $SRCDIR/shared /$SRCFS/child1/shared

log_must zfs create -o checksum=sha256 $SRCFS/child2
log_must dd if=/dev/urandom of=/$SRCFS/child2/unique bs=128K count=8

# Snapshot the entire hierarchy
log_must zfs snapshot -r $SRCFS@snap1

clear_bclone_dedup_stats

# Recursive receive with -B — child datasets should dedup against parent
log_must eval "zfs send -Rc $SRCFS@snap1 | zfs recv -B $DSTFS"

# Verify data integrity for all datasets
log_must diff $SRCDIR/shared $DSTDIR/shared
log_must diff /$SRCFS/child1/shared /$DSTFS/child1/shared
log_must diff /$SRCFS/child2/unique /$DSTFS/child2/unique

# Check stats — child1 should have gotten dedup hits from parent's blocks
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup recursive stats: $stats"

# With cross-dataset auto-tracking, child1's recv should have found
# matches in the parent's blocks.  We expect at least some hits.
if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected dedup hits > 0 with recursive recv, got $hits"
fi

log_pass $claim
