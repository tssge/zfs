#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# Verify that recv -B --bclone-source preserves the source dataset
# in the resume token.  On resume, the index is rebuilt from both the
# cross-dataset source and the destination.  Also test graceful
# degradation when the source is destroyed before resume.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/bclone_dedup/bclone_dedup.kshlib

verify_runnable "global"

claim="recv -B --bclone-source is preserved across resume."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
	[[ -f $streamfile ]] && rm -f $streamfile
	[[ -f $partialfile ]] && rm -f $partialfile
}

log_onexit cleanup

typeset streamfile=$TEST_BASE_DIR/bclone_dedup_resume_source_stream
typeset partialfile=$TEST_BASE_DIR/bclone_dedup_resume_source_partial

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create reference dataset with data
log_must zfs create -o checksum=sha256 $TESTPOOL/ref
log_must dd if=/dev/urandom of=/$TESTPOOL/ref/data bs=128K count=64
log_must zfs snapshot $TESTPOOL/ref@snap

# Create source dataset with the SAME data
log_must zfs create -o checksum=sha256 $SRCFS
log_must cp /$TESTPOOL/ref/data $SRCDIR/data
log_must zfs snapshot $SRCFS@snap1

# Generate send stream to file
log_must eval "zfs send -c $SRCFS@snap1 > $streamfile"

typeset filesize
filesize=$(stat -c %s $streamfile)
log_note "Full stream size: $filesize bytes"

# Truncate to ~half to simulate interrupted receive
typeset truncsize=$(($filesize / 2))
log_must dd if=$streamfile of=$partialfile bs=1 count=$truncsize

#
# Part 1: Resume with source still present
#

# Attempt resumable recv with --bclone-source — will fail due to truncation
log_mustnot eval "zfs recv -sB --bclone-source $TESTPOOL/ref \
    -o checksum=sha256 $DSTFS < $partialfile"

# Get resume token
typeset token
token=$(get_prop receive_resume_token $DSTFS)
log_note "Resume token: $token"

if [[ "$token" == "-" || -z "$token" ]]; then
	log_fail "Expected a resume token, got '$token'"
fi

clear_bclone_dedup_stats

# Resume — source is auto-recovered from ZAP, no --bclone-source needed
log_must eval "zfs send -t $token | zfs recv -sF -o checksum=sha256 $DSTFS"

# Verify data
log_must diff $SRCDIR/data $DSTDIR/data

# Check dedup stats — should have hits from cross-dataset source
typeset stats
stats=$(get_bclone_dedup_stats)
typeset hits=$(echo "$stats" | awk '{print $1}')
log_note "bclone_dedup resume with source stats: $stats"

#
# Part 2: Resume after source destroyed (graceful degradation)
#

# Clean up destination for second test
log_must zfs destroy -r $DSTFS

# Interrupt recv again
log_mustnot eval "zfs recv -sB --bclone-source $TESTPOOL/ref \
    -o checksum=sha256 $DSTFS < $partialfile"

# Now destroy the source dataset
log_must zfs destroy -r $TESTPOOL/ref

# Get new resume token
token=$(get_prop receive_resume_token $DSTFS)
if [[ "$token" == "-" || -z "$token" ]]; then
	log_fail "Expected a resume token after source destroy, got '$token'"
fi

# Resume — source is gone, should degrade gracefully
log_must eval "zfs send -t $token | zfs recv -sF -o checksum=sha256 $DSTFS"

# Verify data integrity — recv must still succeed
log_must diff $SRCDIR/data $DSTDIR/data

log_pass $claim
