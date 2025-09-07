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
#	Test allocation priority property can be set and retrieved on vdevs.
#
# STRATEGY:
#	1. Create a pool with multiple vdevs
#	2. Set different allocation priorities on vdevs
#	3. Verify the properties are correctly set
#	4. Test allocation behavior with different priorities
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

log_assert "Allocation priority property can be set and retrieved"
log_onexit cleanup

# Create test directory and files
mkdir -p $TESTDIR
for file in $TESTFILES; do
	truncate -s 64M $file
done

# Create pool with multiple vdevs
log_must zpool create $TESTPOOL \
    $TESTDIR/file1 $TESTDIR/file2 $TESTDIR/file3

# Test setting allocation priority on different vdevs
log_must zpool set alloc_priority=200 $TESTPOOL $TESTDIR/file1
log_must zpool set alloc_priority=50 $TESTPOOL $TESTDIR/file2
log_must zpool set alloc_priority=0 $TESTPOOL $TESTDIR/file3

# Verify properties are set correctly
alloc_prio1=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file1)
alloc_prio2=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file2)
alloc_prio3=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file3)

log_must test "$alloc_prio1" = "200"
log_must test "$alloc_prio2" = "50"
log_must test "$alloc_prio3" = "0"

log_note "Successfully set allocation priorities: file1=$alloc_prio1, file2=$alloc_prio2, file3=$alloc_prio3"

# Test default value for new vdev
truncate -s 64M $TESTDIR/file4
log_must zpool add $TESTPOOL $TESTDIR/file4
alloc_prio4=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file4)
log_must test "$alloc_prio4" = "100"
log_note "Default allocation priority for new vdev: $alloc_prio4"

# Create some test data to verify allocation is working
log_must zfs create $TESTPOOL/testfs
log_must dd if=/dev/urandom of=/$TESTPOOL/testfs/testfile bs=1M count=10

log_pass "Allocation priority property works correctly"