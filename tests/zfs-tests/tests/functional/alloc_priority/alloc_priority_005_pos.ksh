#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0

#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	Test that allocation priorities persist across pool import/export.
#
# STRATEGY:
#	1. Create a pool with allocation priorities set
#	2. Export the pool
#	3. Import the pool
#	4. Verify allocation priorities are preserved
#	5. Test that allocation behavior still works
#

verify_runnable "global"

TESTPOOL="alloc_priority_pool"
TESTDIR="/tmp/alloc_priority_test"
TESTFILES="$TESTDIR/file1 $TESTDIR/file2 $TESTDIR/file3"

function cleanup
{
	if datasetexists $TESTPOOL ; then
		zpool destroy -f $TESTPOOL
	fi
	rm -rf $TESTDIR
}

log_assert "Allocation priorities persist across pool import/export"
log_onexit cleanup

# Create test directory and files
mkdir -p $TESTDIR
for file in $TESTFILES; do
	truncate -s 64M $file
done

# Create pool with multiple vdevs
log_must zpool create $TESTPOOL \
    $TESTDIR/file1 $TESTDIR/file2 $TESTDIR/file3

# Set different allocation priorities
log_must zpool set alloc_priority=200 $TESTPOOL $TESTDIR/file1
log_must zpool set alloc_priority=100 $TESTPOOL $TESTDIR/file2
log_must zpool set alloc_priority=0 $TESTPOOL $TESTDIR/file3

# Create some data
log_must zfs create $TESTPOOL/testfs
log_must dd if=/dev/urandom of=/$TESTPOOL/testfs/testfile bs=1M count=2

# Record the priorities before export
prio1_before=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file1)
prio2_before=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file2)
prio3_before=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file3)

log_note "Priorities before export: file1=$prio1_before, file2=$prio2_before, file3=$prio3_before"

# Export the pool
log_must zpool export $TESTPOOL

# Import the pool
log_must zpool import $TESTPOOL

# Verify priorities are preserved
prio1_after=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file1)
prio2_after=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file2)
prio3_after=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file3)

log_note "Priorities after import: file1=$prio1_after, file2=$prio2_after, file3=$prio3_after"

log_must test "$prio1_before" = "$prio1_after"
log_must test "$prio2_before" = "$prio2_after"
log_must test "$prio3_before" = "$prio3_after"

# Test that allocation behavior still works
log_must dd if=/dev/urandom of=/$TESTPOOL/testfs/testfile2 bs=1M count=3

# Check that data was allocated (at least one vdev should have space)
vdev1_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file1" | awk '{print $3}')
vdev2_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file2" | awk '{print $3}')
vdev3_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file3" | awk '{print $3}')

log_note "Allocations after import: file1=$vdev1_alloc, file2=$vdev2_alloc, file3=$vdev3_alloc"

# At least one vdev should have allocation
if [[ "$vdev1_alloc" = "0" && "$vdev2_alloc" = "0" && "$vdev3_alloc" = "0" ]]; then
	log_fail "No allocation after pool import"
fi

log_pass "Allocation priorities persist across pool import/export"
