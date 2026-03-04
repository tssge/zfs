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

#
# Copyright (c) 2026, Klara, Inc.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/fiemap/fiemap.kshlib
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
#	Verify FIEMAP reports concrete extents for split indirect remaps.
#	Use ZFS_DEBUG_INDIRECT_REMAP to force remap callbacks through split
#	segments deterministically.
#

verify_runnable "global"

typeset -r BS=$((1024 * 1024))
typeset -r ZFS_FLAGS_PATH="/sys/module/zfs/parameters/zfs_flags"
typeset -r ZFS_DEBUG_INDIRECT_REMAP=$((1 << 10))

typeset old_flags

typeset old_compression
typeset source_base
typeset source_disk

function cleanup
{
	if [[ -n "$old_flags" && -w "$ZFS_FLAGS_PATH" ]]; then
		echo "$old_flags" > "$ZFS_FLAGS_PATH"
	fi

	if datasetexists "$TESTPOOL/$TESTFS" && [[ -n "$old_compression" ]]; then
		log_must zfs set compression=$old_compression $TESTPOOL/$TESTFS
	fi

	fiemap_cleanup
}

log_assert "FIEMAP emits concrete extents for split indirect remaps"
log_onexit cleanup

if ! is_linux; then
	log_unsupported "Linux-specific split remap test"
fi

if [[ ! -w "$ZFS_FLAGS_PATH" ]]; then
	log_unsupported "$ZFS_FLAGS_PATH is not writable"
fi

old_compression=$(get_prop compression $TESTPOOL/$TESTFS)
log_must zfs set compression=off $TESTPOOL/$TESTFS

old_flags=$(cat "$ZFS_FLAGS_PATH")
new_flags=$(printf "0x%08x" \
    $(((old_flags | ZFS_DEBUG_INDIRECT_REMAP) & 0xffffffff)))
log_must eval "echo $new_flags > $ZFS_FLAGS_PATH"

source_disk=$(fiemap_get_last_vdev "$TESTPOOL")
log_must test -n "$source_disk"

source_base=$(fiemap_get_vdev_base "$TESTPOOL" "$source_disk")
log_must test -n "$source_base"

# Place data on the soon-to-be-removed top-level vdev.
fiemap_write $BS 512
fiemap_verify -s -P
log_must fiemap_verify_linearized_range ge_any "$source_base"

log_must zpool remove $TESTPOOL $source_disk
wait_for_removal $TESTPOOL

# Split remap blocks must no longer map in the removed vdev range.
fiemap_verify -s -P
log_must fiemap_verify_linearized_range ge_none "$source_base"

fiemap_remove

log_pass "FIEMAP emits concrete extents for split indirect remaps"
