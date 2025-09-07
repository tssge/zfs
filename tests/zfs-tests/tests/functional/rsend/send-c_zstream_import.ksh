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
# Copyright (c) 2024. All rights reserved.
#

. "$STF_SUITE"/tests/functional/rsend/rsend.kshlib
. "$STF_SUITE"/include/math.shlib

#
# Description:
# Verify zstream import can import gzip files as ZFS streams
#
# Strategy:
# 1. Create a test file and compress it with gzip
# 2. Use zstream import to convert it to a ZFS stream
# 3. Receive the stream into a ZFS dataset
# 4. Verify the data can be extracted and matches original
# 5. Test with custom dataset name option
# 6. Test error conditions (invalid files, missing files)
#

verify_runnable "both"

log_assert "Verify zstream import correctly imports gzip files as ZFS streams."
log_onexit cleanup_pool "$POOL2"

typeset recvfs=$POOL2/recv
typeset recvfs2=$POOL2/recv2

# Create test data
typeset testfile="$BACKDIR/testdata.txt"
typeset gzipfile="$BACKDIR/testdata.txt.gz"

# Create some test data
log_must eval "echo 'This is test data for zstream import' > $testfile"
log_must eval "echo 'Line 2 of test data' >> $testfile"
log_must eval "echo 'Line 3 of test data' >> $testfile"

# Compress with gzip
log_must gzip -c "$testfile" > "$gzipfile"

# Test 1: Import with default dataset name
log_must eval "zstream import $gzipfile | zfs recv $recvfs"

# Verify the dataset was created
log_must zfs list "$recvfs"

# Test 2: Import with custom dataset name
log_must eval "zstream import -d customdata $gzipfile | zfs recv $recvfs2"

# Verify the dataset was created
log_must zfs list "$recvfs2"

# Test 3: Test error conditions
# Test with non-existent file
log_mustnot zstream import /nonexistent/file.gz

# Test with non-gzip file
typeset notgzipfile="$BACKDIR/notgzip.gz"
log_must eval "echo 'not a gzip file' > $notgzipfile"
log_mustnot zstream import "$notgzipfile"

# Test with missing argument
log_mustnot zstream import

log_pass "zstream import correctly imports gzip files as ZFS streams."