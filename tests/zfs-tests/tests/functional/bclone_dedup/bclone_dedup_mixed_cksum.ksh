#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B handles mixed checksum algorithms correctly in an
# incremental stream.  When the source's checksum property changes between
# snapshots, the stream contains records with different drr_checksumtype
# values.  Only records matching the destination's checksum algorithm get
# dedup treatment; mismatched records fall through to normal writes.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B handles incremental streams with mixed checksum algorithms."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create source with sha256 and write initial data
log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=16
log_must zfs snapshot $SRCFS@snap1

# Receive snap1 to populate destination (sha256)
log_must eval "zfs send -c $SRCFS@snap1 | zfs recv -o checksum=sha256 $DSTFS"

# Change source checksum to blake3, write more data
log_must zfs set checksum=blake3 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file2 bs=128K count=16
log_must zfs snapshot $SRCFS@snap2

# Clear stats
clear_bclone_dedup_stats

# Incremental recv with -B — stream has sha256 blocks (from snap1 base)
# and blake3 blocks (new data).  Destination uses sha256, so:
# - sha256 records: can match index → dedup hits
# - blake3 records: algorithm mismatch → misses (fall through to write)
log_must eval "zfs send -c -i $SRCFS@snap1 $SRCFS@snap2 | \
    zfs recv -B $DSTFS"

# Verify data integrity
log_must diff $SRCDIR/file1 $DSTDIR/file1
log_must diff $SRCDIR/file2 $DSTDIR/file2

# Check stats — we expect misses > 0 because blake3 records don't match
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
typeset misses=$(echo "$stats" | awk '{print $2}')
log_note "bclone_dedup stats: $stats (mixed checksum)"

# The incremental stream should have SOME misses from the blake3 records
if [[ "$misses" -eq 0 ]]; then
	log_fail "Expected some misses from blake3 records, got 0"
fi

log_pass $claim
