#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B --bclone-source reports skip reasons to the user.
# When the source cannot be indexed (non-existent, wrong checksum, etc.),
# a warning is printed to stderr.  When successful, verbose mode shows
# "bclone_source: indexed".
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B --bclone-source reports skip reasons."

log_assert $claim

typeset otherpool=${TESTPOOL}x
typeset othervdev=$TEST_BASE_DIR/bclone_dedup_source_warnings_otherpool.vdev

function cleanup
{
	poolexists $otherpool && destroy_pool $otherpool
	[[ -f $othervdev ]] && rm -f $othervdev
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create source dataset with data
log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/data bs=128K count=16
log_must zfs snapshot $SRCFS@snap1

#
# Test 1: --bclone-source pointing at non-existent dataset
#         Should warn "not_found" and recv still succeeds.
#
typeset output
output=$(eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -B --bclone-source $TESTPOOL/nonexistent \
    -o checksum=sha256 $DSTFS" 2>&1)
typeset rc=$?

if [[ $rc -ne 0 ]]; then
	log_fail "recv should succeed even with missing source, got rc=$rc"
fi

if echo "$output" | grep -q "not_found"; then
	log_note "PASS: not_found warning received for non-existent source"
else
	log_fail "Expected 'not_found' warning, got: $output"
fi

log_must diff $SRCDIR/data $DSTDIR/data

#
# Test 2: --bclone-source with non-cryptographic checksum (fletcher4)
#         Should warn "non_crypto_checksum" and recv still succeeds.
#
log_must zfs destroy -r $DSTFS

# Create a reference dataset with fletcher4 (non-crypto) checksum
log_must zfs create -o checksum=fletcher4 $TESTPOOL/fletcher_ref
log_must dd if=/dev/urandom of=/$TESTPOOL/fletcher_ref/data bs=128K count=16
log_must zfs snapshot $TESTPOOL/fletcher_ref@snap

output=$(eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -B --bclone-source $TESTPOOL/fletcher_ref \
    -o checksum=sha256 $DSTFS" 2>&1)
rc=$?

if [[ $rc -ne 0 ]]; then
	log_fail "recv should succeed with non-crypto source, got rc=$rc"
fi

if echo "$output" | grep -q "non_crypto_checksum"; then
	log_note "PASS: non_crypto_checksum warning received"
else
	log_fail "Expected 'non_crypto_checksum' warning, got: $output"
fi

log_must diff $SRCDIR/data $DSTDIR/data

#
# Test 3: --bclone-source with valid source and -v
#         Should show "bclone_source: indexed" in verbose output.
#
log_must zfs destroy -r $DSTFS

# Create a valid reference with sha256
log_must zfs create -o checksum=sha256 $TESTPOOL/ref
log_must cp $SRCDIR/data /$TESTPOOL/ref/data
log_must zfs snapshot $TESTPOOL/ref@snap

output=$(eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -Bv --bclone-source $TESTPOOL/ref \
    -o checksum=sha256 $DSTFS" 2>&1)
rc=$?

if [[ $rc -ne 0 ]]; then
	log_fail "recv should succeed with valid source, got rc=$rc"
fi

if echo "$output" | grep -q "bclone_source: indexed"; then
	log_note "PASS: 'bclone_source: indexed' shown in verbose output"
else
	log_fail "Expected 'bclone_source: indexed' in verbose output, got: $output"
fi

# No warning should appear for successful indexing
if echo "$output" | grep -q "Warning.*bclone-source"; then
	log_fail "Unexpected warning for successful source indexing: $output"
fi

log_must diff $SRCDIR/data $DSTDIR/data

#
# Test 4: --bclone-source in a different pool
#         Should warn "different_pool" and recv still succeeds.
#
log_must zfs destroy -r $DSTFS
log_must truncate -s 512M $othervdev
log_must zpool create -o feature@block_cloning=enabled $otherpool $othervdev
log_must zfs create -o checksum=sha256 $otherpool/ref
log_must cp $SRCDIR/data /$otherpool/ref/data
log_must zfs snapshot $otherpool/ref@snap

output=$(eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -B --bclone-source $otherpool/ref \
    -o checksum=sha256 $DSTFS" 2>&1)
rc=$?

if [[ $rc -ne 0 ]]; then
	log_fail "recv should succeed with cross-pool source, got rc=$rc"
fi

if echo "$output" | grep -q "different_pool"; then
	log_note "PASS: different_pool warning received"
else
	log_fail "Expected 'different_pool' warning, got: $output"
fi

log_must diff $SRCDIR/data $DSTDIR/data
log_must destroy_pool $otherpool
log_must rm -f $othervdev

log_pass $claim
