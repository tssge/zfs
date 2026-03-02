#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B with entirely new data (no overlap with existing
# destination) completes without errors and writes all blocks normally.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B with no overlapping data succeeds with 0 hits."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

log_must zfs create -o checksum=sha256 $SRCFS

# Two different snapshots with completely different data
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=8
log_must zfs snapshot $SRCFS@snap1

# First recv
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -o checksum=sha256 $DSTFS"

# Replace all source data with new random data
log_must rm $SRCDIR/file1
log_must dd if=/dev/urandom of=$SRCDIR/file2 bs=128K count=8
log_must zfs snapshot $SRCFS@snap2

clear_bclone_dedup_stats

# Destroy snapshot from first recv so -F can overwrite
log_must zfs destroy -r $DSTFS@snap1

# Re-recv with entirely different data — should be all misses
log_must eval "zfs send -c $SRCFS@snap2 | \
    zfs recv -BF -o checksum=sha256 $DSTFS"

# Verify data
log_must diff $SRCDIR/file2 $DSTDIR/file2

# Should have 0 hits (all misses)
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
typeset misses=$(echo "$stats" | awk '{print $2}')
log_note "bclone_dedup stats: $stats"

if [[ "$hits" -ne 0 ]]; then
	log_fail "Expected 0 hits with no overlap, got $hits"
fi
if [[ "$misses" -eq 0 ]]; then
	log_fail "Expected misses > 0 with no overlap"
fi

log_pass $claim
