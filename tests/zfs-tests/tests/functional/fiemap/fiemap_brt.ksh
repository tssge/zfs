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
#	Verify FIEMAP reports FIEMAP_EXTENT_SHARED for block-cloned
#	(BRT) extents using exact brt_entry_get_refcount() lookup.
#	Also verify that non-cloned files in the same dataset do NOT
#	report false SHARED flags, even when BRT entries exist nearby.
#
# STRATEGY:
#	1. Create file A with data, clone to B via cp --reflink.
#	2. Verify FIEMAP_EXTENT_SHARED is set on clone B's extents.
#	3. Create non-cloned file C in the same dataset.
#	4. Verify file C has zero SHARED flags (no false positives).
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/fiemap/fiemap.kshlib

verify_runnable "both"

FIEMAP_FILE_NOCLONE="$TESTDIR/fiemap-file-noclone"

function brt_cleanup
{
	rm -f $FIEMAP_FILE_COPY $FIEMAP_FILE_NOCLONE
	fiemap_cleanup
}

log_assert "FIEMAP reports SHARED for block-cloned extents"
log_onexit brt_cleanup

BS=$(get_prop recordsize $TESTPOOL/$TESTFS)

# Block cloning requires the feature to be enabled.
if ! zpool get -H -o value feature@block_cloning $TESTPOOL | \
    grep -q "active\|enabled"; then
	log_unsupported "block_cloning feature not available"
fi

# --- Positive test: cloned extents must have SHARED ---

# Create file A with several blocks of data.
fiemap_write $BS 8
log_must zpool sync $TESTPOOL

# Clone A -> B using reflink.  This creates BRT entries and makes
# the entcount region non-zero for the 16 MB vdev region.
log_must cp --reflink=always $FIEMAP_FILE $FIEMAP_FILE_COPY
log_must zpool sync $TESTPOOL

# Verify the clone reports SHARED extents.  The exact
# brt_entry_get_refcount() lookup guarantees no false positives.
log_must fiemap -s -v -F "shared:>0" $FIEMAP_FILE_COPY

# --- Negative test: non-cloned file must NOT have SHARED ---

# Create file C in the same dataset.  Its DVAs are likely allocated
# in the same vdev region as A/B (within the same 16 MB entcount
# bucket).  With a probabilistic brt_maybe_exists()-only check,
# this would produce false SHARED flags.  With the exact
# brt_entry_get_refcount() lookup, it must report zero.
fiemap_write $BS 8 0 $FIEMAP_FILE_NOCLONE
log_must zpool sync $TESTPOOL

log_must fiemap -s -v -F "shared:=0" $FIEMAP_FILE_NOCLONE

rm -f $FIEMAP_FILE_COPY $FIEMAP_FILE_NOCLONE
fiemap_remove

log_pass "FIEMAP reports SHARED for block-cloned extents"
