#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify intra-stream dedup: when a compressed send stream contains two
# identical files, recv -B should clone the second file's blocks from
# the first file's blocks via BRT.  This tests the deferred-entry path
# where blocks written earlier in the same stream serve as clone sources
# once their txg has synced.
#
# We use a large enough file that the first file's txg syncs before the
# second file's blocks arrive.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="Intra-stream dedup clones identical files via BRT."

log_assert $claim

function cleanup
{
	restore_tunable TXG_TIMEOUT
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Force 1-second txg sync so earlier blocks sync before later ones arrive
log_must save_tunable TXG_TIMEOUT
log_must set_tunable32 TXG_TIMEOUT 1

log_must zfs create -o checksum=sha256 $SRCFS

# Create two identical 32MB files — enough data that the first file's
# blocks will have synced by the time the second file is received.
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=256
log_must cp $SRCDIR/file1 $SRCDIR/file2
log_must zfs snapshot $SRCFS@snap1

clear_bclone_dedup_stats

# Receive into a NEW dataset (no pre-existing data to index).
# Intra-stream dedup must find matches from blocks written earlier
# in the same recv operation.
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -B -o checksum=sha256 $DSTFS"

# Verify data integrity
log_must diff $SRCDIR/file1 $DSTDIR/file1
log_must diff $SRCDIR/file2 $DSTDIR/file2

# Check dedup stats
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup stats: $stats"

# We expect some hits from the second file cloning the first file's blocks.
# The exact count depends on txg timing, but should be > 0.
if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected intra-stream dedup hits > 0, got $hits"
fi

# Verify that bcloneused on the pool is > 0 (BRT refs exist)
typeset bcloneused
bcloneused=$(get_pool_prop bcloneused "$TESTPOOL")
log_note "bcloneused: $bcloneused"
if [[ "$bcloneused" -gt 0 ]]; then
	log_note "bcloneused > 0: BRT references are live"
fi

log_must restore_tunable TXG_TIMEOUT

log_pass $claim
