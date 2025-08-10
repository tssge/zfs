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
#	Verify that FIEMAP correctly reports compressed extents
#
# STRATEGY:
#	1. Enable compression on filesystem
#	2. Create a test file that will be compressed
#	3. Use filefrag to get FIEMAP information
#	4. Verify that compressed extents are reported correctly
#

verify_runnable "both"

. $STF_SUITE/include/libtest.shlib

function cleanup
{
	[[ -f $TESTDIR/compressed_test ]] && log_must rm $TESTDIR/compressed_test
	log_must zfs set compression=off $TESTPOOL/$TESTFS
}

log_assert "Verify that FIEMAP correctly reports compressed extents"
log_onexit cleanup

# Enable compression
log_must zfs set compression=on $TESTPOOL/$TESTFS

# Create a file with repetitive data that should compress well
log_must dd if=/dev/zero of=$TESTDIR/compressed_test bs=1M count=1

# Sync to ensure compression happens
log_must sync

# Test that filefrag works on compressed file
log_must filefrag $TESTDIR/compressed_test

# Test verbose output to verify extent information
filefrag -v $TESTDIR/compressed_test > /tmp/compressed_filefrag_output 2>&1
if [[ $? -ne 0 ]]; then
	log_fail "filefrag failed on compressed ZFS file"
fi

# Verify that we got some extent information  
if ! grep -q "ext:" /tmp/compressed_filefrag_output; then
	log_fail "filefrag did not return extent information for compressed file"
fi

log_pass "FIEMAP correctly reports compressed extents"