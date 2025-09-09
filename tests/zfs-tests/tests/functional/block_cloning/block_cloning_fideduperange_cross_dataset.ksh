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

claim="FIDEDUPERANGE works correctly across different datasets."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Test 1: Cross-dataset deduplication (same pool)
log_must zfs create $TESTPOOL/dataset1
log_must zfs create $TESTPOOL/dataset2

log_must dd if=/dev/urandom of=/$TESTPOOL/dataset1/file1 bs=128K count=4
log_must sync_pool $TESTPOOL

# Copy the file to the other dataset
log_must cp /$TESTPOOL/dataset1/file1 /$TESTPOOL/dataset2/file1
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/dataset1/file1 /$TESTPOOL/dataset2/file1

# Deduplicate across datasets
log_must clonefile -d /$TESTPOOL/dataset1/file1 /$TESTPOOL/dataset2/file1 0 0 524288
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/dataset1/file1 /$TESTPOOL/dataset2/file1

# Verify blocks are shared
typeset blocks=$(get_same_blocks $TESTPOOL/dataset1 file1 $TESTPOOL/dataset2 file1)
log_must [ "$blocks" = "0 1 2 3" ]

# Test 2: Different recordsizes across datasets
log_must zfs set recordsize=64K $TESTPOOL/dataset1
log_must zfs set recordsize=256K $TESTPOOL/dataset2

log_must dd if=/dev/urandom of=/$TESTPOOL/dataset1/file2 bs=64K count=8
log_must sync_pool $TESTPOOL

# Copy the file to the other dataset
log_must cp /$TESTPOOL/dataset1/file2 /$TESTPOOL/dataset2/file2
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/dataset1/file2 /$TESTPOOL/dataset2/file2

# Deduplicate across datasets with different recordsizes
log_must clonefile -d /$TESTPOOL/dataset1/file2 /$TESTPOOL/dataset2/file2 0 0 524288
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/dataset1/file2 /$TESTPOOL/dataset2/file2

# Verify blocks are shared (should use smaller recordsize)
typeset blocks2=$(get_same_blocks $TESTPOOL/dataset1 file2 $TESTPOOL/dataset2 file2)
log_must [ "$blocks2" = "0 1 2 3 4 5 6 7" ]

# Test 3: Encrypted vs unencrypted datasets (should fail)
log_must zfs create -o encryption=on -o keyformat=passphrase -o keylocation=prompt $TESTPOOL/encrypted
log_must zfs create $TESTPOOL/unencrypted

# Set encryption key
echo "password" | log_must zfs load-key $TESTPOOL/encrypted

log_must dd if=/dev/urandom of=/$TESTPOOL/encrypted/file1 bs=128K count=2
log_must cp /$TESTPOOL/encrypted/file1 /$TESTPOOL/unencrypted/file1
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/encrypted/file1 /$TESTPOOL/unencrypted/file1

# Deduplication between encrypted and unencrypted should fail
log_mustnot clonefile -d /$TESTPOOL/encrypted/file1 /$TESTPOOL/unencrypted/file1 0 0 262144

# Test 4: Same encryption key (should work)
log_must zfs create -o encryption=on -o keyformat=passphrase -o keylocation=prompt $TESTPOOL/encrypted2

# Set same encryption key
echo "password" | log_must zfs load-key $TESTPOOL/encrypted2

log_must dd if=/dev/urandom of=/$TESTPOOL/encrypted/file2 bs=128K count=2
log_must cp /$TESTPOOL/encrypted/file2 /$TESTPOOL/encrypted2/file2
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/encrypted/file2 /$TESTPOOL/encrypted2/file2

# Deduplication between datasets with same encryption key should work
log_must clonefile -d /$TESTPOOL/encrypted/file2 /$TESTPOOL/encrypted2/file2 0 0 262144
log_must sync_pool $TESTPOOL

# Verify files still have identical content
log_must have_same_content /$TESTPOOL/encrypted/file2 /$TESTPOOL/encrypted2/file2

# Verify blocks are shared
typeset blocks3=$(get_same_blocks $TESTPOOL/encrypted file2 $TESTPOOL/encrypted2 file2)
log_must [ "$blocks3" = "0 1" ]

# Test 5: Different encryption keys (should fail)
log_must zfs create -o encryption=on -o keyformat=passphrase -o keylocation=prompt $TESTPOOL/encrypted3

# Set different encryption key
echo "different_password" | log_must zfs load-key $TESTPOOL/encrypted3

log_must dd if=/dev/urandom of=/$TESTPOOL/encrypted/file3 bs=128K count=2
log_must cp /$TESTPOOL/encrypted/file3 /$TESTPOOL/encrypted3/file3
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/encrypted/file3 /$TESTPOOL/encrypted3/file3

# Deduplication between datasets with different encryption keys should fail
log_mustnot clonefile -d /$TESTPOOL/encrypted/file3 /$TESTPOOL/encrypted3/file3 0 0 262144

log_pass $claim
