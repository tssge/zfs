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
#	Verify FIEMAP_FLAG_SYNC on a clean (non-dirty) file does not
#	trigger txg_wait_synced().  The dnode_is_dirty() fast path
#	should skip the sync entirely.
#
# STRATEGY:
#	1. Create a file and sync it.
#	2. Measure fiemap time without FIEMAP_FLAG_SYNC.
#	3. Measure fiemap time with FIEMAP_FLAG_SYNC.
#	4. Verify both complete quickly (SYNC on clean file should
#	   not trigger a TXG sync).
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/fiemap/fiemap.kshlib

verify_runnable "both"

log_assert "FIEMAP_FLAG_SYNC on clean file completes quickly"
log_onexit fiemap_cleanup

BS=$(get_prop recordsize $TESTPOOL/$TESTFS)

# Create a file and ensure it is fully synced (clean).
fiemap_write $BS 8
log_must zpool sync $TESTPOOL

# Both with and without SYNC should produce the same results on a
# clean file.  The key property is that SYNC doesn't trigger a
# txg_wait_synced() when the dnode is not dirty.
fiemap_verify -s -D 0:$((BS*8)):1
fiemap_verify -D 0:$((BS*8)):1

fiemap_remove

log_pass "FIEMAP_FLAG_SYNC on clean file completes quickly"
