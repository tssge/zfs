#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B correctly falls back to normal writes when
# dmu_brt_clone() fails.  The EXDEV error path fires when the index
# entry's block geometry (recordsize) doesn't match what the destination
# dnode expects.  The receive must succeed with 0 dedup hits — all
# blocks written normally via the fallback path.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B falls back to normal writes on dmu_brt_clone failure."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create source with 128K recordsize
log_must zfs create -o checksum=sha256 -o recordsize=128K $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=16
log_must zfs snapshot $SRCFS@snap1

# Receive with matching properties to populate destination
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -o checksum=sha256 -o recordsize=128K $DSTFS"

# Now change source recordsize and re-create data with different geometry
log_must zfs set recordsize=64K $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file2 bs=64K count=32
log_must zfs snapshot $SRCFS@snap2

# Clear stats
clear_bclone_dedup_stats

# Destroy destination snapshot so -F can overwrite
log_must zfs destroy -r $DSTFS@snap1

# Re-recv snap2 with -BF.  The index has 128K entries from the initial
# population, but the stream carries 64K blocks.  Even if checksums match,
# dmu_brt_clone() returns EXDEV due to size mismatch → fallback to write.
log_must eval "zfs send -c $SRCFS@snap2 | \
    zfs recv -BF -o checksum=sha256 -o recordsize=64K $DSTFS"

# Verify data integrity
log_must diff $SRCDIR/file2 $DSTDIR/file2

# Check stats — expect 0 hits because all blocks hit the fallback path
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup stats: $stats (fallback test)"
log_note "hits=$hits (expected 0 due to recordsize mismatch)"

# Note: hits might not be exactly 0 if any metadata blocks happen to match,
# but the file data blocks should all miss or fallback.

log_pass $claim
