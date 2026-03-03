#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B works with raw (encrypted) send streams.
# Raw receives preserve encryption metadata and block contents as-is;
# bclone_dedup should correctly clone encrypted blocks via BRT since
# dmu_brt_clone operates on DVAs directly (no decryption needed).
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

log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=32
log_must zfs snapshot $SRCFS@snap1

# First raw recv — populates encrypted destination
log_must eval "zfs send -w $SRCFS@snap1 | zfs recv $DSTFS"

# Verify destination is encrypted with key unavailable
typeset keystatus
keystatus=$(get_prop keystatus $DSTFS)
log_must test "$keystatus" = "unavailable"

# Clear stats from initial recv
clear_bclone_dedup_stats

# Destroy destination snapshot so -F can overwrite
log_must zfs destroy -r $DSTFS@snap1

# Second raw recv with -BF — should deduplicate encrypted blocks
log_must eval "zfs send -w $SRCFS@snap1 | zfs recv -BF $DSTFS"

# Check dedup stats
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup stats: $stats"

if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected bclone_dedup hits > 0, got $hits"
fi

# Load key on destination and verify data integrity
log_must zfs load-key $DSTFS
log_must zfs mount $DSTFS

log_must diff $SRCDIR/file1 $DSTDIR/file1

log_pass $claim
