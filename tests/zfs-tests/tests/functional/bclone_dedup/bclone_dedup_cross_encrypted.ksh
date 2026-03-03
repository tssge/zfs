#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B --bclone-source works when the source is an
# encrypted dataset.  Encrypted datasets skip the checksum type
# validation (they use embedded MACs) and should be indexed normally.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B --bclone-source works with encrypted source."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
	[[ -f $keyfile ]] && rm -f $keyfile
}

log_onexit cleanup

typeset keyfile=$TEST_BASE_DIR/bclone_dedup_enc_key

# Create encryption key
log_must eval "echo 'password123password123password1' > $keyfile"

log_must zpool create -o feature@block_cloning=enabled \
    -o feature@encryption=enabled $TESTPOOL $DISKS

# Create encrypted reference dataset
log_must zfs create -o encryption=aes-256-gcm \
    -o keyformat=passphrase -o keylocation=file://$keyfile \
    -o checksum=sha256 $TESTPOOL/enc_ref
log_must dd if=/dev/urandom of=/$TESTPOOL/enc_ref/data bs=128K count=16
log_must zfs snapshot $TESTPOOL/enc_ref@snap

# Create unencrypted source with the SAME data
log_must zfs create -o checksum=sha256 $SRCFS
log_must cp /$TESTPOOL/enc_ref/data $SRCDIR/data
log_must zfs snapshot $SRCFS@snap1

clear_bclone_dedup_stats

# Receive with --bclone-source pointing at the encrypted dataset.
# The encrypted source should be indexed (skips checksum type check).
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -B --bclone-source $TESTPOOL/enc_ref \
    -o checksum=sha256 $DSTFS"

# Verify data integrity
log_must diff $SRCDIR/data $DSTDIR/data

# Check stats — encrypted source blocks may not match unencrypted
# stream blocks (different on-disk format), but the indexing itself
# should not error out.
typeset stats
stats=$(get_bclone_dedup_stats)
log_note "bclone_dedup cross-encrypted stats: $stats"

# The recv should complete successfully regardless of hit count.
# Encrypted blocks have different physical representations, so
# hits depend on whether the checksums actually match.

log_pass $claim
