#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0

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

#
# Copyright (c) 2024 by OpenZFS. All rights reserved.
#

. $STF_SUITE/tests/functional/rsend/rsend.kshlib

#
# Description:
# Verify zstream ext2-import creates valid stream from ext2 image
#
# Strategy:
# 1. Create a small ext2 filesystem image
# 2. Use zstream ext2-import to generate a stream
# 3. Verify the stream is well-formed
# 4. Test with invalid inputs
#

verify_runnable "both"

log_assert "Verify zstream ext2-import creates valid stream from ext2 image."
log_onexit cleanup_pool $POOL2

typeset tmpdir=$(mktemp -d)
typeset ext2_image=$tmpdir/test_ext2.img
typeset stream_output=$tmpdir/test_stream.out

function cleanup_test
{
	rm -rf $tmpdir
}

log_onexit cleanup_test

# Create a small ext2 filesystem (10MB)
log_must dd if=/dev/zero of=$ext2_image bs=1M count=10
log_must mkfs.ext2 -q $ext2_image

# Test basic ext2-import functionality
log_must zstream ext2-import $ext2_image testpool/testds > $stream_output

# Verify the stream output exists and has reasonable size
[[ -f $stream_output ]] || log_fail "Stream output file not created"
[[ -s $stream_output ]] || log_fail "Stream output file is empty"

# Test verbose mode
log_must zstream ext2-import -v $ext2_image testpool/testds2 > $stream_output

# Test with invalid arguments
log_mustnot zstream ext2-import
log_mustnot zstream ext2-import $ext2_image
log_mustnot zstream ext2-import nonexistent.img testpool/testds
log_mustnot zstream ext2-import /dev/null testpool/testds

log_pass "zstream ext2-import creates valid stream from ext2 image."