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
# Verify zstream import progress reporting and large file handling
#
# Strategy:
# 1. Create a large test file (>1MB) and compress it with gzip
# 2. Test progress reporting with verbose mode
# 3. Test chunked reading for large files
# 4. Verify the stream is created correctly
# 5. Test with different compression levels on large files
#

verify_runnable "both"

log_assert "Verify zstream import handles large files and shows progress correctly."
log_onexit cleanup_pool "$POOL2"

typeset recvfs=$POOL2/recv

# Create a large test file (>1MB to trigger progress reporting)
typeset largefile="$BACKDIR/large_data.txt"
typeset largegzip="$BACKDIR/large_data.txt.gz"

# Create a 2MB file with compressible data
log_must eval "dd if=/dev/zero bs=1024 count=2048 2>/dev/null | tr '\0' 'A' > $largefile"
log_must test -s "$largefile"

# Compress with gzip
log_must gzip -c "$largefile" > "$largegzip"
log_must test -s "$largegzip"

# Test 1: Test progress reporting with verbose mode
log_must eval "zstream import -v $largegzip > $BACKDIR/large.stream 2> $BACKDIR/progress_output"
log_must test -s "$BACKDIR/progress_output"

# Verify progress output contains expected information
log_must grep -q "Creating ZFS stream" "$BACKDIR/progress_output"
log_must grep -q "File size:" "$BACKDIR/progress_output"
log_must grep -q "Processing.*bytes" "$BACKDIR/progress_output"
log_must grep -q "ZFS stream created successfully" "$BACKDIR/progress_output"

# Test 2: Verify the stream is valid
log_must test -s "$BACKDIR/large.stream"
typeset stream_output=$(zstream dump "$BACKDIR/large.stream" 2>/dev/null)
log_must test -n "$stream_output"

# Verify stream structure
echo "$stream_output" | log_must grep -q "Total DRR_WRITE records = 1"
echo "$stream_output" | log_must grep -q "Total payload size ="

# Verify payload size matches original gzip file
typeset original_size=$(stat -c%s "$largegzip")
typeset stream_payload_size=$(echo "$stream_output" | grep "Total payload size" | sed 's/.* = \([0-9]*\).*/\1/')
log_must test "$stream_payload_size" -eq "$original_size"

# Test 3: Test with different compression levels on large files
log_must eval "zstream import -l 1 -v $largegzip > $BACKDIR/large_level1.stream 2> $BACKDIR/level1_progress"
log_must eval "zstream import -l 9 -v $largegzip > $BACKDIR/large_level9.stream 2> $BACKDIR/level9_progress"

# Both should show progress reporting
log_must grep -q "Processing.*bytes" "$BACKDIR/level1_progress"
log_must grep -q "Processing.*bytes" "$BACKDIR/level9_progress"

# Both streams should be valid
log_must test -s "$BACKDIR/large_level1.stream"
log_must test -s "$BACKDIR/large_level9.stream"

# Test 4: Test that the stream can be received
log_must eval "zfs recv $recvfs < $BACKDIR/large.stream"
log_must zfs list "$recvfs"

# Test 5: Test with a very large file (but not too large to avoid test timeouts)
# Create a 5MB file
typeset verylargefile="$BACKDIR/very_large_data.txt"
typeset verylargegzip="$BACKDIR/very_large_data.txt.gz"

log_must eval "dd if=/dev/zero bs=1024 count=5120 2>/dev/null | tr '\0' 'B' > $verylargefile"
log_must gzip -c "$verylargefile" > "$verylargegzip"

# Test progress reporting on very large file
log_must eval "zstream import -v $verylargegzip > $BACKDIR/very_large.stream 2> $BACKDIR/very_large_progress"
log_must grep -q "Processing.*bytes" "$BACKDIR/very_large_progress"

# Test 6: Test file size limit (should fail for files > 1GB)
# Create a file that's just over 1GB (but we'll simulate this with a smaller file
# to avoid actually creating a 1GB file in tests)
typeset hugefile="$BACKDIR/huge.gz"
# Create a file that's larger than our 1GB limit by creating a sparse file
# (This is a simulation - in real usage, the limit would be enforced)
log_must eval "dd if=/dev/zero of=$hugefile bs=1 count=0 seek=1073741825 2>/dev/null"
# This should fail due to our size limit
log_mustnot zstream import "$hugefile"

# Test 7: Test that progress reporting doesn't interfere with stream output
# The progress should go to stderr, stream to stdout
log_must eval "zstream import -v $largegzip > $BACKDIR/clean_stream 2> $BACKDIR/clean_progress"
log_must test -s "$BACKDIR/clean_stream"
log_must test -s "$BACKDIR/clean_progress"

# The stream should be identical to non-verbose version
log_must eval "zstream import $largegzip > $BACKDIR/non_verbose_stream"
log_must cmp "$BACKDIR/clean_stream" "$BACKDIR/non_verbose_stream"

log_pass "zstream import handles large files and shows progress correctly."
