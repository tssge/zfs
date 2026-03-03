#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that "zfs recv -Bv" prints bclone_dedup statistics in its verbose
# output.  The stats (hits, misses, index entries) are returned by the
# kernel via the recv ioctl outnvl and displayed by libzfs.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -Bv prints bclone_dedup statistics in verbose output."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=32
log_must zfs snapshot $SRCFS@snap1

# First recv — populates destination
log_must eval "zfs send -c $SRCFS@snap1 | zfs recv -o checksum=sha256 $DSTFS"

# Destroy snapshot so -F can overwrite
log_must zfs destroy -r $DSTFS@snap1

# Second recv with -BFv — capture verbose output
typeset output
output=$(zfs send -c $SRCFS@snap1 | zfs recv -BFv -o checksum=sha256 $DSTFS 2>&1)
typeset rc=$?

if [[ $rc -ne 0 ]]; then
	log_fail "recv -BFv failed with rc=$rc: $output"
fi

log_note "Verbose output: $output"

# Verify the output contains the bclone_dedup stats line
echo "$output" | grep -q "^bclone_dedup:" || \
    log_fail "Verbose output missing 'bclone_dedup:' stats line"

# Verify it contains the expected fields
echo "$output" | grep -q "blocks cloned" || \
    log_fail "Verbose output missing 'blocks cloned'"
echo "$output" | grep -q "misses" || \
    log_fail "Verbose output missing 'misses'"
echo "$output" | grep -q "index entries" || \
    log_fail "Verbose output missing 'index entries'"

# Parse and verify stats are reasonable
typeset stats
stats=$(get_bclone_dedup_verbose_stats "$output")
typeset hits=$(echo "$stats" | awk '{print $1}')
typeset misses=$(echo "$stats" | awk '{print $2}')
typeset entries=$(echo "$stats" | awk '{print $3}')

log_note "Parsed verbose stats: hits=$hits misses=$misses entries=$entries"

if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected bclone_dedup hits > 0, got $hits"
fi

if [[ "$entries" -eq 0 ]]; then
	log_fail "Expected index entries > 0, got $entries"
fi

# Also verify the standard recv verbose line is present
echo "$output" | grep -q "received.*stream in.*seconds" || \
    log_fail "Verbose output missing standard 'received ... stream' line"

# Verify data integrity
log_must diff $SRCDIR/file1 $DSTDIR/file1

log_pass $claim
