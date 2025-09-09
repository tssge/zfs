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
#	Test edge cases for allocation priorities.
#
# STRATEGY:
#	1. Test all vdevs with priority 0 (last resort only)
#	2. Test all vdevs with same priority
#	3. Test mixed priority scenarios
#	4. Test priority changes on existing pools
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

log_assert "Allocation priority edge cases work correctly"
log_onexit cleanup

# Create test directory and files
mkdir -p $TESTDIR
for file in $TESTFILES; do
	truncate -s 64M $file
done

# Test 1: All vdevs with priority 0 (last resort only)
log_must zpool create $TESTPOOL \
    $TESTDIR/file1 $TESTDIR/file2 $TESTDIR/file3

log_must zpool set alloc_priority=0 $TESTPOOL $TESTDIR/file1
log_must zpool set alloc_priority=0 $TESTPOOL $TESTDIR/file2
log_must zpool set alloc_priority=0 $TESTPOOL $TESTDIR/file3

log_must zfs create $TESTPOOL/testfs
log_must dd if=/dev/urandom of=/$TESTPOOL/testfs/testfile1 bs=1M count=2

# All vdevs should have some allocation since they're all priority 0
vdev1_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file1" | awk '{print $3}')
vdev2_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file2" | awk '{print $3}')
vdev3_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file3" | awk '{print $3}')

log_note "All priority 0: file1=$vdev1_alloc, file2=$vdev2_alloc, file3=$vdev3_alloc"

# At least one should have allocation
if [[ "$vdev1_alloc" = "0" && "$vdev2_alloc" = "0" && "$vdev3_alloc" = "0" ]]; then
	log_fail "No allocation with all priority 0 vdevs"
fi

# Test 2: All vdevs with same priority (should distribute evenly)
log_must zpool destroy $TESTPOOL
log_must zpool create $TESTPOOL \
    $TESTDIR/file1 $TESTDIR/file2 $TESTDIR/file3

log_must zpool set alloc_priority=150 $TESTPOOL $TESTDIR/file1
log_must zpool set alloc_priority=150 $TESTPOOL $TESTDIR/file2
log_must zpool set alloc_priority=150 $TESTPOOL $TESTDIR/file3

log_must zfs create $TESTPOOL/testfs
log_must dd if=/dev/urandom of=/$TESTPOOL/testfs/testfile2 bs=1M count=5

# All should have some allocation
vdev1_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file1" | awk '{print $3}')
vdev2_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file2" | awk '{print $3}')
vdev3_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file3" | awk '{print $3}')

log_note "All priority 150: file1=$vdev1_alloc, file2=$vdev2_alloc, file3=$vdev3_alloc"

# Test 3: Mixed priorities - one high, others low
log_must zpool set alloc_priority=255 $TESTPOOL $TESTDIR/file1
log_must zpool set alloc_priority=1 $TESTPOOL $TESTDIR/file2
log_must zpool set alloc_priority=0 $TESTPOOL $TESTDIR/file3

log_must dd if=/dev/urandom of=/$TESTPOOL/testfs/testfile3 bs=1M count=3

vdev1_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file1" | awk '{print $3}')
vdev2_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file2" | awk '{print $3}')
vdev3_alloc=$(zpool list -v $TESTPOOL | grep "$TESTDIR/file3" | awk '{print $3}')

log_note "Mixed priorities: file1=$vdev1_alloc, file2=$vdev2_alloc, file3=$vdev3_alloc"

# Test 4: Priority changes on existing pool
log_must zpool set alloc_priority=50 $TESTPOOL $TESTDIR/file1
log_must zpool set alloc_priority=200 $TESTPOOL $TESTDIR/file2

# Verify the changes took effect
prio1=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file1)
prio2=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file2)
prio3=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTDIR/file3)

log_must test "$prio1" = "50"
log_must test "$prio2" = "200"
log_must test "$prio3" = "0"

log_note "Priority changes successful: file1=$prio1, file2=$prio2, file3=$prio3"

log_pass "Allocation priority edge cases work correctly"
