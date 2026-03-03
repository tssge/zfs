#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B correctly handles send streams containing
# DRR_WRITE_EMBEDDED records.  Embedded blocks are stored inline
# in the block pointer and cannot be BRT-cloned (bclone_dedup.c
# skips BP_IS_EMBEDDED).  The recv should complete without error
# and data should be intact.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B handles embedded blocks correctly."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled \
    -o feature@embedded_data=enabled $TESTPOOL $DISKS

# Create source with compressible data that will produce embedded blocks.
# Small, highly compressible files trigger embedded_data.
log_must zfs create -o checksum=sha256 -o compression=lz4 $SRCFS

# Write small compressible files (likely embedded) and one larger file
for i in $(seq 1 20); do
	log_must eval "printf '%0.s0' {1..512} > $SRCDIR/small_$i"
done
# Also write a normal-sized file for non-embedded blocks
log_must dd if=/dev/urandom of=$SRCDIR/large bs=128K count=8

log_must zfs snapshot $SRCFS@snap1

# First recv with -ce (compressed + embedded)
log_must eval "zfs send -ce $SRCFS@snap1 | \
    zfs recv -o checksum=sha256 -o compression=lz4 $DSTFS"

# Verify data
for i in $(seq 1 20); do
	log_must diff $SRCDIR/small_$i $DSTDIR/small_$i
done
log_must diff $SRCDIR/large $DSTDIR/large

# Destroy snapshot and re-recv with -BF
log_must zfs destroy -r $DSTFS@snap1

clear_bclone_dedup_stats

log_must eval "zfs send -ce $SRCFS@snap1 | \
    zfs recv -BF -o checksum=sha256 -o compression=lz4 $DSTFS"

# Verify data intact
for i in $(seq 1 20); do
	log_must diff $SRCDIR/small_$i $DSTDIR/small_$i
done
log_must diff $SRCDIR/large $DSTDIR/large

# Stats: large file blocks should have hits, embedded blocks are skipped
typeset stats
stats=$(get_bclone_dedup_stats)
log_note "bclone_dedup embedded stats: $stats"

log_pass $claim
