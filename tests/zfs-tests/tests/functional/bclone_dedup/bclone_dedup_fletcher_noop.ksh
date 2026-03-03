#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B produces zero dedup hits when the existing destination
# data was written with a different cryptographic checksum algorithm than the
# source.  The index entries (blake3) won't match the stream records (sha256)
# because the AVL comparator checks checksum type first.  Both algorithms
# are cryptographic, so the upfront validation passes — but the per-record
# algorithm match causes all lookups to miss.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B with cryptographic algorithm mismatch degrades gracefully (0 hits)."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Source uses sha256
log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=8
log_must zfs snapshot $SRCFS@snap1

# Create destination INDEPENDENTLY with blake3 and different data.
# This populates the bdi index with blake3 entries.
log_must zfs create -o checksum=blake3 $DSTFS
log_must dd if=/dev/urandom of=$DSTDIR/dummy bs=128K count=8
log_must sync_pool $TESTPOOL

clear_bclone_dedup_stats

# Recv with -BF overwrites the destination.
# Index has blake3 entries from the old destination data.
# Stream has sha256 records from the source.
# AVL lookup: sha256 key vs blake3 entries → type mismatch → 0 hits.
log_must eval "zfs send -c $SRCFS@snap1 | zfs recv -BF -o checksum=sha256 $DSTFS"

# Should succeed but with 0 hits
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup stats: $stats"

if [[ "$hits" -ne 0 ]]; then
	log_fail "Expected 0 hits with blake3/sha256 mismatch, got $hits"
fi

# Data integrity must still hold
log_must diff $SRCDIR/file1 $DSTDIR/file1

log_pass $claim
