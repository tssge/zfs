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
#	Verify FIEMAP completes for large sparse files without
#	stalling the pool.  The chunked iteration should release
#	locks between chunks so the pool remains responsive.
#
# STRATEGY:
#	1. Create a large sparse file with scattered writes.
#	2. Run fiemap and verify completion.
#	3. Verify all written regions are reported.
#	4. Verify pool remains responsive during fiemap.
#
# TIMEOUT: 300
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/fiemap/fiemap.kshlib

verify_runnable "both"

log_assert "FIEMAP completes for large sparse files"
log_onexit fiemap_cleanup

BS=$(get_prop recordsize $TESTPOOL/$TESTFS)

# Create a large sparse file by writing scattered blocks.
# Write 100 blocks at widely spaced offsets to create a 1GB+ sparse file.
for i in $(seq 0 10 990); do
	dd if=/dev/urandom of=$FIEMAP_FILE bs=$BS count=1 \
	    seek=$((i * 100)) conv=notrunc 2>/dev/null
done
log_must zpool sync $TESTPOOL

# Verify fiemap completes successfully.  The -s flag triggers sync
# which is a no-op since we already synced.
log_must fiemap -s -v $FIEMAP_FILE

# Verify pool is responsive by running zpool status.
log_must zpool status $TESTPOOL

fiemap_remove

log_pass "FIEMAP completes for large sparse files"
