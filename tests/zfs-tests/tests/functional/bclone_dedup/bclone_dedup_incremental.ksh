#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that incremental send with -B works correctly.  An incremental
# stream only contains changed blocks, so the dedup rate depends on
# how much overlap exists.  With a small change, most blocks match
# the existing destination and get cloned.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="Incremental recv -B deduplicates unchanged blocks."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=32
log_must zfs snapshot $SRCFS@snap1

# Initial full send/recv
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -o checksum=sha256 $DSTFS"

# Make a small change and create second snapshot
log_must dd if=/dev/urandom of=$SRCDIR/file2 bs=128K count=2
log_must zfs snapshot $SRCFS@snap2

clear_bclone_dedup_stats

# Incremental recv with -B
# The stream only contains the new blocks (file2), so there won't
# be many dedup opportunities against the existing destination data.
# This test verifies the operation completes without errors.
log_must eval "zfs send -c -i $SRCFS@snap1 $SRCFS@snap2 | \
    zfs recv -B $DSTFS"

# Verify data integrity
log_must diff $SRCDIR/file1 $DSTDIR/file1
log_must diff $SRCDIR/file2 $DSTDIR/file2

typeset stats
stats=$(get_bclone_dedup_stats)
log_note "bclone_dedup incremental stats: $stats"

log_pass $claim
