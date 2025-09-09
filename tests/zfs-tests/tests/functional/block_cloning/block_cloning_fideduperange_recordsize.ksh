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

claim="FIDEDUPERANGE works correctly with different recordsize values."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Test 1: Small recordsize (512 bytes)
log_must zfs create $TESTPOOL/small
log_must zfs set recordsize=512 $TESTPOOL/small

log_must dd if=/dev/urandom of=/$TESTPOOL/small/file1 bs=512 count=8
log_must sync_pool $TESTPOOL

# Copy the file to create identical content
log_must cp /$TESTPOOL/small/file1 /$TESTPOOL/small/file2
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/small/file1 /$TESTPOOL/small/file2

# Deduplicate the entire files
log_must clonefile -d /$TESTPOOL/small/file1 /$TESTPOOL/small/file2 0 0 4096
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/small/file1 /$TESTPOOL/small/file2

# Verify blocks are shared
typeset blocks=$(get_same_blocks $TESTPOOL/small file1 $TESTPOOL/small file2)
log_must [ "$blocks" = "0 1 2 3 4 5 6 7" ]

# Test 2: Large recordsize (1MB)
log_must zfs create $TESTPOOL/large
log_must zfs set recordsize=1M $TESTPOOL/large

log_must dd if=/dev/urandom of=/$TESTPOOL/large/file1 bs=1M count=2
log_must sync_pool $TESTPOOL

# Copy the file to create identical content
log_must cp /$TESTPOOL/large/file1 /$TESTPOOL/large/file2
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/large/file1 /$TESTPOOL/large/file2

# Deduplicate the entire files
log_must clonefile -d /$TESTPOOL/large/file1 /$TESTPOOL/large/file2 0 0 2097152
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/large/file1 /$TESTPOOL/large/file2

# Verify blocks are shared
typeset blocks2=$(get_same_blocks $TESTPOOL/large file1 $TESTPOOL/large file2)
log_must [ "$blocks2" = "0 1" ]

# Test 3: Very large recordsize (16MB) - tests chunk size scaling
log_must zfs create $TESTPOOL/huge
log_must zfs set recordsize=16M $TESTPOOL/huge

log_must dd if=/dev/urandom of=/$TESTPOOL/huge/file1 bs=16M count=1
log_must sync_pool $TESTPOOL

# Copy the file to create identical content
log_must cp /$TESTPOOL/huge/file1 /$TESTPOOL/huge/file2
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/huge/file1 /$TESTPOOL/huge/file2

# Deduplicate the entire files
log_must clonefile -d /$TESTPOOL/huge/file1 /$TESTPOOL/huge/file2 0 0 16777216
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/huge/file1 /$TESTPOOL/huge/file2

# Verify blocks are shared
typeset blocks3=$(get_same_blocks $TESTPOOL/huge file1 $TESTPOOL/huge file2)
log_must [ "$blocks3" = "0" ]

# Test 4: Different recordsizes between files (should use smaller)
log_must zfs create $TESTPOOL/mixed
log_must zfs set recordsize=4K $TESTPOOL/mixed

# Create files with different block sizes
log_must dd if=/dev/urandom of=/$TESTPOOL/mixed/file1 bs=4K count=4
log_must dd if=/dev/urandom of=/$TESTPOOL/mixed/file2 bs=8K count=2
log_must sync_pool $TESTPOOL

# Make file2 identical to file1
log_must cp /$TESTPOOL/mixed/file1 /$TESTPOOL/mixed/file2
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/mixed/file1 /$TESTPOOL/mixed/file2

# Deduplicate the entire files
log_must clonefile -d /$TESTPOOL/mixed/file1 /$TESTPOOL/mixed/file2 0 0 16384
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/mixed/file1 /$TESTPOOL/mixed/file2

# Verify blocks are shared (should use 4K blocks)
typeset blocks4=$(get_same_blocks $TESTPOOL/mixed file1 $TESTPOOL/mixed file2)
log_must [ "$blocks4" = "0 1 2 3" ]

log_pass $claim
