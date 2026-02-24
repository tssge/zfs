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
#	Verify FIEMAP does not crash or hang when run concurrently
#	with writes to the same file.  This stress-tests the locking
#	rework (copy-and-release dn_mtx) and chunked iteration.
#
# STRATEGY:
#	1. Create a file with data.
#	2. Start background writers overwriting random blocks.
#	3. Run fiemap repeatedly in a loop.
#	4. Verify: no crashes, no hangs, no kernel warnings.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/fiemap/fiemap.kshlib

verify_runnable "both"

log_assert "FIEMAP is safe under concurrent writes"
log_onexit fiemap_concurrent_cleanup

WRITER_PID=""

function fiemap_concurrent_cleanup
{
	[[ -n "$WRITER_PID" ]] && kill $WRITER_PID 2>/dev/null
	wait $WRITER_PID 2>/dev/null
	fiemap_cleanup
}

BS=$(get_prop recordsize $TESTPOOL/$TESTFS)

# Create a moderately sized file.
fiemap_write $BS 64
log_must zpool sync $TESTPOOL

# Start background writer that continuously overwrites random blocks.
(
	while true; do
		BLOCK=$((RANDOM % 64))
		dd if=/dev/urandom of=$FIEMAP_FILE bs=$BS count=1 \
		    seek=$BLOCK conv=notrunc 2>/dev/null
	done
) &
WRITER_PID=$!

# Run fiemap repeatedly.  Each invocation should succeed (exit 0)
# even though the file is being modified concurrently.
for i in $(seq 1 20); do
	log_must fiemap -s -v $FIEMAP_FILE
done

# Stop the writer.
kill $WRITER_PID 2>/dev/null
wait $WRITER_PID 2>/dev/null
WRITER_PID=""

# Verify no kernel warnings.
log_mustnot dmesg | grep -q "WARNING.*fiemap"

fiemap_remove

log_pass "FIEMAP is safe under concurrent writes"
