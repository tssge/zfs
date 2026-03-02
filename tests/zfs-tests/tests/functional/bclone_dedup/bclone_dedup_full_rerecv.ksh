#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that a full re-receive with -BF deduplicates blocks that already
# exist on the destination.  The second recv should clone blocks from the
# first recv via BRT instead of re-writing them.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="Full re-receive with -BF deduplicates against existing destination."

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

# First recv — populates destination
log_must eval "zfs send -c $SRCFS@snap1 | zfs recv -o checksum=sha256 $DSTFS"

# Clear stats from the initial recv
clear_bclone_dedup_stats

# Destroy snapshot from first recv so -F can overwrite
log_must zfs destroy -r $DSTFS@snap1

# Second recv with -BF — should deduplicate via block cloning
log_must eval "zfs send -c $SRCFS@snap1 | zfs recv -BF -o checksum=sha256 $DSTFS"

# Verify data integrity
log_must diff $SRCDIR/file1 $DSTDIR/file1

# Check dedup stats — should have hits
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup stats: $stats"

if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected bclone_dedup hits > 0, got $hits"
fi

log_pass $claim
