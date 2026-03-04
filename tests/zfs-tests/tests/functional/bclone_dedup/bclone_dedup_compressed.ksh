#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B works with compressed send streams (-c).
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B works with compressed send streams."

log_assert $claim

typeset srcvol=$TESTPOOL/srcvol
typeset dstvol=$TESTPOOL/dstvol
typeset srcvoldev=$ZVOL_DEVDIR/$srcvol
typeset dstvoldev=$ZVOL_DEVDIR/$dstvol
typeset voldatain=$TEST_BASE_DIR/bclone_dedup_compressed_vol_input
typeset voldataout=$TEST_BASE_DIR/bclone_dedup_compressed_vol_output

function cleanup
{
	[[ -f $voldatain ]] && rm -f $voldatain
	[[ -f $voldataout ]] && rm -f $voldataout
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

log_must zfs create -o checksum=sha256 -o compression=lz4 $SRCFS
log_must dd if=/dev/urandom of=$SRCDIR/file1 bs=128K count=16
log_must zfs snapshot $SRCFS@snap1

# First recv
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -o checksum=sha256 -o compression=lz4 $DSTFS"

clear_bclone_dedup_stats

# Destroy snapshot from first recv so -F can overwrite
log_must zfs destroy -r $DSTFS@snap1

# Re-recv with -BF
log_must eval "zfs send -c $SRCFS@snap1 | \
    zfs recv -BF -o checksum=sha256 -o compression=lz4 $DSTFS"

# Verify data
log_must diff $SRCDIR/file1 $DSTDIR/file1

typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup stats: $stats"

if [[ "$hits" -eq 0 ]]; then
	log_fail "Expected dedup hits > 0 with compressed send, got $hits"
fi

#
# zvol coverage: recv -B with compressed streams should also work
# for volume datasets.
#
log_must dd if=/dev/urandom of=$voldatain bs=1M count=16
log_must zfs create -V 64M -o checksum=sha256 -o compression=lz4 $srcvol
block_device_wait $srcvoldev
log_must dd if=$voldatain of=$srcvoldev bs=1M count=16
log_must zfs snapshot $srcvol@snap1

log_must eval "zfs send -c $srcvol@snap1 | \
    zfs recv -o checksum=sha256 $dstvol"

clear_bclone_dedup_stats
log_must zfs destroy -r $dstvol@snap1
log_must eval "zfs send -c $srcvol@snap1 | \
    zfs recv -BF -o checksum=sha256 $dstvol"

block_device_wait $dstvoldev
log_must dd if=$dstvoldev of=$voldataout bs=1M count=16
log_must cmp $voldatain $voldataout

log_pass $claim
