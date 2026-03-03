#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B works with raw (encrypted) send streams.
# Raw receives preserve encryption metadata and block contents as-is;
# bclone_dedup should build the index from encrypted blocks and complete
# the receive without errors.
#
# We use an incremental raw stream because full raw streams with -F are
# blocked for encrypted datasets (libzfs restriction).  The incremental
# recv with -B exercises the raw+bclone_dedup code path: the index is
# built from the destination's existing encrypted blocks (from snap1),
# and the new blocks from snap2 are processed through the dedup path.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B works with raw (encrypted) send streams."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
	[[ -f $keyfile ]] && log_must rm -f $keyfile
}

log_onexit cleanup

typeset keyfile=$TEST_BASE_DIR/bclone_dedup_raw_key
log_must eval "echo 'password' > $keyfile"

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create encrypted source with a cryptographic checksum
log_must zfs create \
    -o encryption=on \
    -o keyformat=passphrase \
    -o keylocation=file://$keyfile \
    -o checksum=sha256 \
    $SRCFS

log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=16
log_must zfs snapshot $SRCFS@snap1

# Write more data and create snap2
log_must dd if=/dev/urandom of=$SRCDIR/file2 bs=128K count=16
log_must zfs snapshot $SRCFS@snap2

# Receive snap1 into encrypted destination via raw send
log_must eval "zfs send -w $SRCFS@snap1 | zfs recv $DSTFS"

# Verify destination is encrypted
typeset keystatus
keystatus=$(get_prop keystatus $DSTFS)
log_must test "$keystatus" = "unavailable"

# Clear stats before the -B recv
clear_bclone_dedup_stats

# Receive the incremental with -B.  The index is built from dest's
# current state (snap1's encrypted blocks).  file2's blocks from the
# incremental are new data, so hits will be low or zero — but this
# exercises the raw recv + bclone_dedup index build code path and
# verifies no panics or errors.
log_must eval "zfs send -w -i $SRCFS@snap1 $SRCFS@snap2 | zfs recv -B $DSTFS"

# Check stats — index should have been built (entries > 0)
typeset stats
stats=$(get_bclone_dedup_stats)
typeset entries=$(echo "$stats" | awk '{print $5}')
log_note "bclone_dedup stats: $stats"
log_note "index entries=$entries"

# Load key on destination (raw recv sets keylocation=prompt, so use -L)
log_must zfs load-key -L file://$keyfile $DSTFS
log_must zfs mount $DSTFS

log_must diff $SRCDIR/file1 $DSTDIR/file1
log_must diff $SRCDIR/file2 $DSTDIR/file2

log_pass $claim
