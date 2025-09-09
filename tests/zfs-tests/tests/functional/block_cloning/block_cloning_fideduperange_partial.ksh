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
# Copyright (c) 2023, Klara Inc.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/block_cloning/block_cloning.kshlib

verify_runnable "global"

claim="The FIDEDUPERANGE ioctl can deduplicate partial file ranges."

log_assert $claim

function cleanup
{
	datasetexists $TESTPOOL && destroy_pool $TESTPOOL
}

log_onexit cleanup

log_must zpool create -o feature@block_cloning=enabled $TESTPOOL $DISKS

# Create a larger file for partial range testing
log_must dd if=/dev/urandom of=/$TESTPOOL/file1 bs=128K count=8
log_must sync_pool $TESTPOOL

# Copy the file to create identical content
log_must cp /$TESTPOOL/file1 /$TESTPOOL/file2
log_must sync_pool $TESTPOOL

# Verify files have identical content
log_must have_same_content /$TESTPOOL/file1 /$TESTPOOL/file2

# Modify the beginning of file2 to make it different
log_must dd if=/dev/urandom of=/$TESTPOOL/file2 bs=128K count=2 conv=notrunc
log_must sync_pool $TESTPOOL

# Now deduplicate only the identical portion (latter half)
# Offset 262144 = 128K * 2, Length 524288 = 128K * 4
log_must clonefile -d /$TESTPOOL/file1 /$TESTPOOL/file2 262144 262144 524288
log_must sync_pool $TESTPOOL

# Verify the deduped portion has shared blocks
# Only blocks 2,3,4,5 should be shared (latter half of 8 blocks total)
typeset blocks=$(get_same_blocks $TESTPOOL file1 $TESTPOOL file2)
log_must [ "$blocks" = "2 3 4 5" ]

log_pass $claim