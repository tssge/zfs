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
# fields enclosed by brackets "[]" replaced with your own identifying
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

claim="FIDEDUPERANGE handles unaligned ranges correctly."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Test 1: Unaligned ranges that can be aligned
log_must dd if=/dev/urandom of=/$TESTPOOL/file1 bs=128K count=8
log_must sync_pool $TESTPOOL

# Copy the file to create identical content
log_must cp /$TESTPOOL/file1 /$TESTPOOL/file2
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/file1 /$TESTPOOL/file2

# Test unaligned range that spans multiple blocks
# Start at offset 64K (half block), length 256K (2 blocks)
# This should align to block boundaries and deduplicate 2 blocks
log_must clonefile -d /$TESTPOOL/file1 /$TESTPOOL/file2 65536 65536 262144
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/file1 /$TESTPOOL/file2

# Verify blocks are shared (blocks 1 and 2 should be shared)
typeset blocks=$(get_same_blocks $TESTPOOL file1 $TESTPOOL file2)
log_must [ "$blocks" = "1 2" ]

# Test 2: Range too small to align (should return 0 bytes)
log_must dd if=/dev/urandom of=/$TESTPOOL/file3 bs=128K count=2
log_must cp /$TESTPOOL/file3 /$TESTPOOL/file4
log_must sync_pool $TESTPOOL

# Try to deduplicate a very small unaligned range (1KB at offset 1KB)
# This should return 0 bytes because it's too small to align
log_must clonefile -d /$TESTPOOL/file3 /$TESTPOOL/file4 1024 1024 1024
log_must sync_pool $TESTPOOL

# Verify no blocks are shared since the range was too small
typeset blocks2=$(get_same_blocks $TESTPOOL file3 $TESTPOOL file4)
log_must [ -z "$blocks2" ]

# Test 3: Unaligned range at end of file
log_must dd if=/dev/urandom of=/$TESTPOOL/file5 bs=128K count=3
log_must cp /$TESTPOOL/file5 /$TESTPOOL/file6
log_must sync_pool $TESTPOOL

# Try to deduplicate from middle of last block to end
# Offset 200K (middle of 3rd block), length 88K (rest of file)
log_must clonefile -d /$TESTPOOL/file5 /$TESTPOOL/file6 204800 204800 90112
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/file5 /$TESTPOOL/file6

# Verify no blocks are shared since the range was too small to align
typeset blocks3=$(get_same_blocks $TESTPOOL file5 $TESTPOOL file6)
log_must [ -z "$blocks3" ]

log_pass $claim
