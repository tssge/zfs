#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B --bclone-source works for cross-dataset dedup.
# The index is built from a different dataset (the "reference") and
# blocks in the incoming stream that match are cloned via BRT.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B --bclone-source deduplicates across datasets."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create the "reference" dataset with data
log_must zfs create -o checksum=sha256 $TESTPOOL/ref
log_must dd if=/dev/urandom of=/$TESTPOOL/ref/data bs=128K count=32
log_must zfs snapshot $TESTPOOL/ref@snap

# Create the source dataset with the SAME data
log_must zfs create -o checksum=sha256 $SRCFS
log_must cp /$TESTPOOL/ref/data $SRCDIR/data
log_must zfs snapshot $SRCFS@snap1

clear_bclone_dedup_stats

# Receive with --bclone-source pointing at the reference dataset
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv --bclone-source $TESTPOOL/ref -B \
    -o checksum=sha256 $DSTFS"

# Verify data integrity
log_must diff $SRCDIR/data $DSTDIR/data

# Verify dedup occurred — blocks should match the reference
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup cross-dataset stats: $stats"

if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected dedup hits > 0 with cross-dataset source, got $hits"
fi

log_pass $claim
