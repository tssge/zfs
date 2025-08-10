#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2024. All rights reserved.
#

#
# DESCRIPTION:
#	Verify that FIEMAP ioctl works correctly on ZFS
#
# STRATEGY:
#	1. Create a test file
#	2. Use filefrag to get FIEMAP information
#	3. Verify that the command succeeds and returns extents
#

verify_runnable "both"

. $STF_SUITE/include/libtest.shlib

function cleanup
{
	[[ -f $TESTDIR/fiemap_test ]] && log_must rm $TESTDIR/fiemap_test
}

log_assert "Verify that FIEMAP ioctl works correctly on ZFS"
log_onexit cleanup

# Create a test file
log_must dd if=/dev/urandom of=$TESTDIR/fiemap_test bs=1M count=1

# Test that filefrag works (basic FIEMAP functionality)
log_must filefrag $TESTDIR/fiemap_test

# Test verbose output 
filefrag -v $TESTDIR/fiemap_test > /tmp/filefrag_output 2>&1
if [[ $? -ne 0 ]]; then
	log_fail "filefrag failed on ZFS file"
fi

# Verify that we got some extent information
if ! grep -q "ext:" /tmp/filefrag_output; then
	log_fail "filefrag did not return extent information"
fi

log_pass "FIEMAP ioctl works correctly on ZFS"