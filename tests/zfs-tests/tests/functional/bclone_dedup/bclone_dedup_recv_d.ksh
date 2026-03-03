#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -d -B works correctly.  The -d flag strips the first
# element of the send name and appends the rest to the destination.
# Dedup should still work against the destination's own blocks.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -d -B works with prefix stripping."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create source dataset with data
log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/data bs=128K count=16
log_must zfs snapshot $SRCFS@snap1

# First recv with -d — creates $TESTPOOL/backup/src
log_must zfs create $TESTPOOL/backup
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -d -o checksum=sha256 $TESTPOOL/backup"

# Verify data arrived at the right place
log_must diff $SRCDIR/data /$TESTPOOL/backup/src/data

# Destroy snapshot and re-recv with -d -BF
log_must zfs destroy -r $TESTPOOL/backup/src@snap1

clear_bclone_dedup_stats

log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -d -BF -o checksum=sha256 $TESTPOOL/backup"

# Verify data
log_must diff $SRCDIR/data /$TESTPOOL/backup/src/data

# Check dedup stats
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup recv -d stats: $stats"

if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected dedup hits > 0 with recv -d, got $hits"
fi

log_pass $claim
