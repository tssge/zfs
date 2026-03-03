#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -e -B works correctly.  The -e flag uses only the
# last element of the send name appended to the destination.
# Dedup should still work against the destination's own blocks.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -e -B works with tail mode."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create a deeper hierarchy: pool/a/b/src
log_must zfs create $TESTPOOL/a
log_must zfs create $TESTPOOL/a/b
log_must zfs create -o checksum=sha256 $TESTPOOL/a/b/src
log_must dd if=/dev/urandom of=/$TESTPOOL/a/b/src/data bs=128K count=16
log_must zfs snapshot $TESTPOOL/a/b/src@snap1

# First recv with -e — creates $TESTPOOL/backup/src (tail only)
log_must zfs create $TESTPOOL/backup
log_must eval "zfs send -c $TESTPOOL/a/b/src@snap1 | \
    zfs recv -e -o checksum=sha256 $TESTPOOL/backup"

# Verify data arrived at the right place
log_must diff /$TESTPOOL/a/b/src/data /$TESTPOOL/backup/src/data

# Destroy snapshot and re-recv with -e -BF
log_must zfs destroy -r $TESTPOOL/backup/src@snap1

clear_bclone_dedup_stats

log_must eval "zfs send -c $TESTPOOL/a/b/src@snap1 | \
    zfs recv -e -BF -o checksum=sha256 $TESTPOOL/backup"

# Verify data
log_must diff /$TESTPOOL/a/b/src/data /$TESTPOOL/backup/src/data

# Check dedup stats
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup recv -e stats: $stats"

if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected dedup hits > 0 with recv -e, got $hits"
fi

log_pass $claim
