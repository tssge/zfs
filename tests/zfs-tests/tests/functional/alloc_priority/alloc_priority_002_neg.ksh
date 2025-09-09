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
#	Test that invalid allocation priority values are rejected.
#
# STRATEGY:
#	1. Create a pool with a vdev
#	2. Try to set invalid allocation priority values (> 255)
#	3. Verify the commands fail with appropriate error messages
#	4. Verify the property remains unchanged
#

verify_runnable "global"

TESTPOOL="alloc_priority_pool"
TESTDIR="/tmp/alloc_priority_test"
TESTFILE="$TESTDIR/file1"

function cleanup
{
	if datasetexists $TESTPOOL ; then
		zpool destroy -f $TESTPOOL
	fi
	rm -rf $TESTDIR
}

log_assert "Invalid allocation priority values are rejected"
log_onexit cleanup

# Create test directory and file
mkdir -p $TESTDIR
truncate -s 64M $TESTFILE

# Create pool with a vdev
log_must zpool create $TESTPOOL $TESTFILE

# Set a valid priority first
log_must zpool set alloc_priority=100 $TESTPOOL $TESTFILE

# Test invalid values (> 255)
log_mustnot zpool set alloc_priority=256 $TESTPOOL $TESTFILE
log_mustnot zpool set alloc_priority=1000 $TESTPOOL $TESTFILE
log_mustnot zpool set alloc_priority=4294967295 $TESTPOOL $TESTFILE

# Verify the property is still the original value
alloc_prio=$(zpool get -Hp -o value alloc_priority $TESTPOOL $TESTFILE)
log_must test "$alloc_prio" = "100"

log_pass "Invalid allocation priority values are properly rejected"
