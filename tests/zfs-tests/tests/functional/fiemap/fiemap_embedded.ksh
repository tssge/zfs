#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
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
# CDDL HEADER END
#

# DESCRIPTION:
#	Verify FIEMAP reports non-zero physical offset for embedded
#	block pointers.  Embedded BPs store data inline in the dnode;
#	the physical offset should reflect the dnode block location.
#
# STRATEGY:
#	1. Create a small file (< 112 bytes for embedded BP).
#	2. Sync to ensure embedded BP is created.
#	3. Run fiemap and verify FIEMAP_EXTENT_DATA_INLINE is set.
#	4. Verify physical offset is non-zero (dnode block location).
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/fiemap/fiemap.kshlib

verify_runnable "both"

log_assert "FIEMAP reports embedded BP physical offset"
log_onexit fiemap_cleanup

# Use a small recordsize and ensure compression is on (required for
# embedded block pointers).
log_must zfs set recordsize=131072 $TESTPOOL/$TESTFS

# Write a small amount of data that will produce an embedded BP.
# 64 bytes is well under the 112-byte embedded BP limit.
log_must dd if=/dev/urandom of=$FIEMAP_FILE bs=64 count=1
log_must zpool sync $TESTPOOL

# Verify the extent has DATA_INLINE flag and non-zero physical offset.
fiemap_verify -s -P -D 0:64:1 -F "inline:1"

fiemap_remove

log_pass "FIEMAP reports embedded BP physical offset"
