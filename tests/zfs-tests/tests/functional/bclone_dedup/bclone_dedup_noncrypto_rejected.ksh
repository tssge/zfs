#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B is rejected upfront when the existing destination
# dataset uses a non-cryptographic checksum (e.g. fletcher4).  This is a
# correctness requirement: non-cryptographic checksums are NOT collision-
# resistant, and cloning based on a false match would cause silent data
# corruption.  The kernel must reject the recv before building the index.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B rejected when destination uses non-cryptographic checksum."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create source with sha256 (cryptographic)
log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=8
log_must zfs snapshot $SRCFS@snap1

# Create destination with fletcher4 (non-cryptographic) and populate it
log_must zfs create -o checksum=fletcher4 $DSTFS
log_must dd if=/dev/urandom of=$DSTDIR/dummy bs=128K count=8
log_must zfs snapshot $DSTFS@snap1

# Attempt recv -BF into fletcher4 destination — must fail with ENOTSUP
log_must zfs destroy -r $DSTFS@snap1
log_mustnot eval "zfs send -c $SRCFS@snap1 | zfs recv -BF $DSTFS"

log_pass $claim
