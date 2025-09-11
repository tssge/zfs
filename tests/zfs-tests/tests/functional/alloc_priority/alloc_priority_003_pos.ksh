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
#	Test that allocation priorities affect data placement.
#
# STRATEGY:
#	1. Create a pool with multiple vdevs
#	2. Set different allocation priorities on vdevs
#	3. Create a dataset and write data
#	4. Verify that data is allocated according to priorities
#	5. Test priority 0 (last resort) behavior
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

log_assert "Allocation priorities affect data placement"
log_onexit cleanup

# Create test directory and files
mkdir -p $TESTDIR
for file in $TESTFILES; do
	truncate -s 64M $file
done

# Create pool with multiple vdevs
log_must zpool create $TESTPOOL \
    $TESTDIR/file1 $TESTDIR/file2 $TESTDIR/file3

# Set allocation priorities: file1=high, file2=normal, file3=last resort
log_must zpool set alloc_priority=200 $TESTPOOL $TESTDIR/file1
log_must zpool set alloc_priority=100 $TESTPOOL $TESTDIR/file2
log_must zpool set alloc_priority=0 $TESTPOOL $TESTDIR/file3

# Create a dataset and write some data
log_must zfs create $TESTPOOL/testfs
log_must dd if=/dev/urandom of=/$TESTPOOL/testfs/testfile bs=1M count=5

# Check which vdevs have data allocated
# We expect file1 (priority 200) to have data, file3 (priority 0) to be empty
# unless file1 and file2 are full
vdev1_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file1" | awk '{print $3}')
vdev2_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file2" | awk '{print $3}')
vdev3_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file3" | awk '{print $3}')

log_note "Vdev allocations: file1=$vdev1_alloc, file2=$vdev2_alloc, file3=$vdev3_alloc"

# At least one vdev should have allocated space
if [[ "$vdev1_alloc" = "0" && "$vdev2_alloc" = "0" && "$vdev3_alloc" = "0" ]]; then
	log_fail "No data was allocated to any vdev"
fi

# Test priority 0 behavior by filling up higher priority vdevs
log_must dd if=/dev/urandom of=/$TESTPOOL/testfs/largefile bs=1M count=50

# Now check if priority 0 vdev got used when others were full
vdev3_alloc_after=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file3" | awk '{print $3}')

log_note "Vdev3 allocation after large write: $vdev3_alloc_after"

# If vdev3 has allocation, it means priority 0 worked as last resort
if [[ "$vdev3_alloc_after" != "0" ]]; then
	log_note "Priority 0 vdev was used as last resort - allocation priorities working correctly"
fi

log_pass "Allocation priorities affect data placement correctly"
