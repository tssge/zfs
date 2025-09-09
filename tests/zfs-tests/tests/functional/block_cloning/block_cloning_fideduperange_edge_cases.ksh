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
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced by your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2025, Klara Inc.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/block_cloning/block_cloning.kshlib

verify_runnable "global"

claim="FIDEDUPERANGE handles edge cases correctly."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Test 1: Zero-length range (should succeed with 0 bytes)
log_must dd if=/dev/urandom of=/$TESTPOOL/file1 bs=128K count=4
log_must cp /$TESTPOOL/file1 /$TESTPOOL/file2
log_must sync_pool $TESTPOOL

# Zero length should succeed but process 0 bytes
log_must clonefile -d /$TESTPOOL/file1 /$TESTPOOL/file2 0 0 0
log_must sync_pool $TESTPOOL

# Verify no blocks are shared since 0 bytes were processed
typeset blocks=$(get_same_blocks $TESTPOOL file1 $TESTPOOL file2)
log_must [ -z "$blocks" ]

# Test 2: Same file, same range (should succeed with full length)
log_must dd if=/dev/urandom of=/$TESTPOOL/file3 bs=128K count=4
log_must sync_pool $TESTPOOL

# Deduplicate same file, same range
log_must clonefile -d /$TESTPOOL/file3 /$TESTPOOL/file3 0 0 524288
log_must sync_pool $TESTPOOL

# This should succeed and return the full length
# (though no actual deduplication occurs since it's the same range)

# Test 3: Overlapping ranges in same file (should fail with EINVAL)
log_must dd if=/dev/urandom of=/$TESTPOOL/file4 bs=128K count=8
log_must sync_pool $TESTPOOL

# Try to deduplicate overlapping ranges in the same file
# This should fail because the ranges overlap
log_mustnot clonefile -d /$TESTPOOL/file4 /$TESTPOOL/file4 0 131072 262144
log_must sync_pool $TESTPOOL

# Test 4: Range extending beyond file size
log_must dd if=/dev/urandom of=/$TESTPOOL/file5 bs=128K count=2
log_must cp /$TESTPOOL/file5 /$TESTPOOL/file6
log_must sync_pool $TESTPOOL

# Try to deduplicate beyond file size (should be truncated)
log_must clonefile -d /$TESTPOOL/file5 /$TESTPOOL/file6 0 0 1048576
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/file5 /$TESTPOOL/file6

# Verify blocks are shared (should be truncated to file size)
typeset blocks2=$(get_same_blocks $TESTPOOL file5 $TESTPOOL file6)
log_must [ "$blocks2" = "0 1" ]

# Test 5: Empty files
log_must touch /$TESTPOOL/empty1 /$TESTPOOL/empty2
log_must sync_pool $TESTPOOL

# Deduplicate empty files
log_must clonefile -d /$TESTPOOL/empty1 /$TESTPOOL/empty2 0 0 0
log_must sync_pool $TESTPOOL

# Test 6: Very small files (smaller than block size)
log_must dd if=/dev/urandom of=/$TESTPOOL/small1 bs=1K count=1
log_must cp /$TESTPOOL/small1 /$TESTPOOL/small2
log_must sync_pool $TESTPOOL

# Deduplicate small files
log_must clonefile -d /$TESTPOOL/small1 /$TESTPOOL/small2 0 0 1024
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/small1 /$TESTPOOL/small2

# Test 7: Offset beyond file size
log_must dd if=/dev/urandom of=/$TESTPOOL/file7 bs=128K count=2
log_must cp /$TESTPOOL/file7 /$TESTPOOL/file8
log_must sync_pool $TESTPOOL

# Try to deduplicate starting beyond file size (should return 0 bytes)
log_must clonefile -d /$TESTPOOL/file7 /$TESTPOOL/file8 1048576 1048576 1048576
log_must sync_pool $TESTPOOL

# Verify no blocks are shared since offset was beyond file size
typeset blocks3=$(get_same_blocks $TESTPOOL file7 $TESTPOOL file8)
log_must [ -z "$blocks3" ]

# Test 8: Different file sizes
log_must dd if=/dev/urandom of=/$TESTPOOL/file9 bs=128K count=4
log_must dd if=/dev/urandom of=/$TESTPOOL/file10 bs=128K count=2
log_must sync_pool $TESTPOOL

# Make file10 identical to first half of file9
log_must dd if=/$TESTPOOL/file9 of=/$TESTPOOL/file10 bs=128K count=2
log_must sync_pool $TESTPOOL

# Deduplicate the common portion
log_must clonefile -d /$TESTPOOL/file9 /$TESTPOOL/file10 0 0 262144
log_must sync_pool $TESTPOOL

# Verify files still have identical content in the common portion
log_must have_same_content /$TESTPOOL/file9 /$TESTPOOL/file10

# Verify blocks are shared
typeset blocks4=$(get_same_blocks $TESTPOOL file9 $TESTPOOL file10)
log_must [ "$blocks4" = "0 1" ]

log_pass $claim
