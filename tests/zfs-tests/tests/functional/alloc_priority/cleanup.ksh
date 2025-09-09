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
# Cleanup for allocation priority tests
#

# Clean up any remaining pools
for pool in $(zpool list -H -o name 2>/dev/null | grep alloc_priority); do
	zpool destroy -f $pool 2>/dev/null || true
done

# Clean up test directory
TESTDIR="/tmp/alloc_priority_test"
rm -rf $TESTDIR

log_pass "Allocation priority test cleanup completed"
