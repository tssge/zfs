#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B works with large block send streams (-L).
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B works with large block send streams."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled \
    -o feature@large_blocks=enabled $TESTPOOL $DISKS

log_must zfs create -o checksum=sha256 -o recordsize=1M $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=1M count=16
log_must zfs snapshot $SRCFS@snap1

# First recv with large blocks
log_must eval "zfs send -Lc $SRCFS@snap1 | \
    zfs recv -o checksum=sha256 -o recordsize=1M $DSTFS"

clear_bclone_dedup_stats

# Destroy and re-recv with -BF
log_must zfs destroy -r $DSTFS@snap1

log_must eval "zfs send -Lc $SRCFS@snap1 | \
    zfs recv -BF -o checksum=sha256 -o recordsize=1M $DSTFS"

# Verify data
log_must diff $SRCDIR/file1 $DSTDIR/file1

typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup large_blocks stats: $stats"

if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected dedup hits > 0 with large blocks, got $hits"
fi

log_pass $claim
