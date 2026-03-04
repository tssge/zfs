#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B is rejected when the destination dataset to be created
# would use a non-cryptographic checksum (e.g. fletcher4).  This must be
# rejected for newfs receives as well, not only when the destination already
# exists.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B rejected for new destination with non-cryptographic checksum."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Source stream is compressed and checksum-safe.
log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=8
log_must zfs snapshot $SRCFS@snap1

#
# Case 1: explicit non-crypto checksum via -o on a new destination.
#
log_mustnot eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -B -o checksum=fletcher4 $DSTFS"

if datasetexists $DSTFS; then
	log_fail "Destination dataset should not exist after rejected recv -B"
fi

#
# Case 2: inherited non-crypto checksum from parent on a new destination.
#
log_must zfs create -o checksum=fletcher4 $TESTPOOL/noncrypto_parent

log_mustnot eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -B $TESTPOOL/noncrypto_parent/dst"

if datasetexists $TESTPOOL/noncrypto_parent/dst; then
	log_fail "Inherited non-crypto destination should be rejected for recv -B"
fi

log_pass $claim
