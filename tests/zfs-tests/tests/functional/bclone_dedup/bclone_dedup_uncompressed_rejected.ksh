#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B rejects uncompressed (plain) send streams with
# an appropriate error.  Only raw (-w) and compressed (-c) streams
# are supported because uncompressed streams don't preserve the
# source block checksums needed for dedup matching.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B rejects uncompressed send streams."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=4
log_must zfs snapshot $SRCFS@snap1

# Plain send (no -c, no -w) should fail with -B on new destination
log_mustnot eval "zfs send $SRCFS@snap1 | \
    zfs recv -B -o checksum=sha256 $DSTFS"

# Populate destination so recv -B builds a pre-index from existing blocks
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -o checksum=sha256 $DSTFS"

# Plain incremental stream should still be rejected with -B
log_must dd if=/dev/urandom of=$SRCDIR/file2 bs=128K count=4
log_must zfs snapshot $SRCFS@snap2

log_mustnot eval "zfs send -i $SRCFS@snap1 $SRCFS@snap2 | \
    zfs recv -BF $DSTFS"

log_pass $claim
