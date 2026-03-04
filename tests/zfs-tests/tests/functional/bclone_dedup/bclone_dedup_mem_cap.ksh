#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B completes successfully when the memory cap
# (zfs_recv_bclone_dedup_max_bytes) is set very low.  With a tiny cap
# only a few index entries fit, so most blocks will miss and fall through
# to normal writes.  The receive must still succeed — the cap limits
# dedup effectiveness, not correctness.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B completes correctly with a very low memory cap."

log_assert $claim

function cleanup
{
	restore_tunable RECV_BCLONE_DEDUP_MAX_BYTES
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

log_must zfs create -o checksum=sha256 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=32
log_must zfs snapshot $SRCFS@snap1

# First recv — populates destination
log_must eval "zfs send -c $SRCFS@snap1 | zfs recv -o checksum=sha256 $DSTFS"

# Set memory cap very low (~4KB, allows roughly 16 index entries)
log_must save_tunable RECV_BCLONE_DEDUP_MAX_BYTES
log_must set_tunable64 RECV_BCLONE_DEDUP_MAX_BYTES 4096

# Clear stats
clear_bclone_dedup_stats

# Destroy snapshot so -F can overwrite
log_must zfs destroy -r $DSTFS@snap1

# Re-recv with -BF under tight memory cap
log_must eval "zfs send -c $SRCFS@snap1 | zfs recv -BF -o checksum=sha256 $DSTFS"

# Verify data integrity
log_must diff $SRCDIR/file1 $DSTDIR/file1

# Check stats — hits should be less than total blocks (cap limits coverage)
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
typeset entries=$(echo "$stats" | awk '{print $5}')
log_note "bclone_dedup stats with 4KB cap: $stats"

# With a 4KB cap, the index can hold very few entries
# Some hits are possible if the first few blocks match, but not all
log_note "hits=$hits entries=$entries (expected entries to be capped)"

if [[ "$entries" -gt 64 ]]; then
	log_fail "Expected index entries capped by memory, got $entries"
fi

log_pass $claim
