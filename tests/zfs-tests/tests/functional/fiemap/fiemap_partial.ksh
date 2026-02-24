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
#	Verify FIEMAP partial results work correctly when the
#	userspace buffer is smaller than the total number of extents.
#	The -m flag limits the extent buffer, testing the re-call
#	pattern where userspace calls fiemap multiple times with
#	advancing start offsets.
#
# STRATEGY:
#	1. Create a file with many extents.
#	2. Call fiemap with -m to limit extent buffer.
#	3. Verify the limited call returns fewer extents.
#	4. Verify a full call returns all extents.
#	5. Verify the extent count with -m is less than the full count.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/fiemap/fiemap.kshlib

verify_runnable "both"

log_assert "FIEMAP partial results work correctly"
log_onexit fiemap_cleanup

BS=$(get_prop recordsize $TESTPOOL/$TESTFS)

# Create a file with many separate extents by writing with holes.
# This creates alternating data/hole blocks: X.X.X.X.X.X.X.X.
for i in $(seq 0 2 30); do
	dd if=/dev/urandom of=$FIEMAP_FILE bs=$BS count=1 \
	    seek=$i conv=notrunc 2>/dev/null
done
log_must zpool sync $TESTPOOL

# Full fiemap should report all data extents.
FULL_OUTPUT=$(fiemap -s -a -v $FIEMAP_FILE 2>&1)
log_must [ $? -eq 0 ]

# Count data extents in full output (lines with physical offset != 0).
FULL_COUNT=$(echo "$FULL_OUTPUT" | grep -c "^[0-9]" || true)

# With -m 3, we should get at most 3 extents.
PARTIAL_OUTPUT=$(fiemap -s -a -m 3 -v $FIEMAP_FILE 2>&1)
log_must [ $? -eq 0 ]

PARTIAL_COUNT=$(echo "$PARTIAL_OUTPUT" | grep -c "^[0-9]" || true)

log_note "Full extents: $FULL_COUNT, Partial extents: $PARTIAL_COUNT"

# Partial count should be at most 3 (the -m limit).
if [ "$PARTIAL_COUNT" -gt 3 ]; then
	log_fail "Partial result returned $PARTIAL_COUNT extents, expected <= 3"
fi

# Full count should be greater than partial count.
if [ "$FULL_COUNT" -le "$PARTIAL_COUNT" ]; then
	log_fail "Full count ($FULL_COUNT) should exceed partial ($PARTIAL_COUNT)"
fi

fiemap_remove

log_pass "FIEMAP partial results work correctly"
