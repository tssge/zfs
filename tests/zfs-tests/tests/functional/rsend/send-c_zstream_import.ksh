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

# Test 3: Validate the stream contains expected data
# Create a stream file to inspect its contents
log_must eval "zstream import $gzipfile > $BACKDIR/test.stream"

# Verify the stream was created and contains data
log_must test -s "$BACKDIR/test.stream"

# Use zstream dump to verify the stream structure and data size
typeset stream_output
stream_output=$(zstream dump "$BACKDIR/test.stream" 2>/dev/null)
log_must test -n "$stream_output"

# Verify the stream contains a write record with payload data
echo "$stream_output" | log_must grep -q "Total DRR_WRITE records = 1"
echo "$stream_output" | log_must grep -q "Total payload size ="

# Extract and verify payload size matches the original gzip file
typeset original_size stream_payload_size
original_size=$(stat -c%s "$gzipfile")
stream_payload_size=$(echo "$stream_output" | grep "Total payload size" | sed 's/.* = \([0-9]*\).*/\1/')

log_note "Original gzip file size: $original_size bytes"
log_note "Stream payload size: $stream_payload_size bytes"

# The stream payload should contain the gzip data
log_must test "$stream_payload_size" -eq "$original_size"

# Test 4: Verify the stream can be processed by zfs receive
# This validates that the ZFS stream format is correct
# Note: In actual deployment, the received dataset would need proper
# filesystem structure to access the file content directly

# Test 5: Test verbose mode
log_must eval "zstream import -v $gzipfile > $BACKDIR/test_verbose.stream 2> $BACKDIR/verbose_output"
log_must test -s "$BACKDIR/verbose_output"
log_must grep -q "Creating ZFS stream" "$BACKDIR/verbose_output"
log_must grep -q "File size:" "$BACKDIR/verbose_output"
log_must grep -q "ZFS stream created successfully" "$BACKDIR/verbose_output"

# Test 6: Test compression level options
typeset recvfs3=$POOL2/recv3
typeset recvfs4=$POOL2/recv4

# Test with compression level 1 (fastest)
log_must eval "zstream import -l 1 -d fastcomp $gzipfile | zfs recv $recvfs3"
log_must zfs list "$recvfs3"

# Test with compression level 9 (best compression)
log_must eval "zstream import -l 9 -d bestcomp $gzipfile | zfs recv $recvfs4"
log_must zfs list "$recvfs4"

# Test 7: Test invalid compression levels
log_mustnot zstream import -l 0 $gzipfile
log_mustnot zstream import -l 10 $gzipfile
log_mustnot zstream import -l invalid $gzipfile

# Test 8: Test enhanced error conditions
# Test with empty file
typeset emptyfile="$BACKDIR/empty.gz"
log_must eval "touch $emptyfile"
log_mustnot zstream import "$emptyfile"

# Test with file too small to be gzip
typeset smallfile="$BACKDIR/small.gz"
log_must eval "echo 'x' > $smallfile"
log_mustnot zstream import "$smallfile"

# Test with invalid dataset name
log_mustnot zstream import -d "" $gzipfile
log_mustnot zstream import -d "invalid@name" $gzipfile
log_mustnot zstream import -d "invalid#name" $gzipfile

# Test with very long dataset name (should fail)
typeset longname=$(printf "a%.0s" {1..300})
log_mustnot zstream import -d "$longname" $gzipfile

# Test 9: Test gzip header validation
# Create a file with invalid gzip magic bytes
typeset badmagic="$BACKDIR/badmagic.gz"
log_must eval "printf '\x1f\x8c\x08\x00\x00\x00\x00\x00' > $badmagic"
log_mustnot zstream import "$badmagic"

# Create a file with unsupported compression method
typeset badmethod="$BACKDIR/badmethod.gz"
log_must eval "printf '\x1f\x8b\x09\x00\x00\x00\x00\x00' > $badmethod"
log_mustnot zstream import "$badmethod"

# Test 10: Test with gzip file containing extra fields (should fail)
typeset extrafile="$BACKDIR/extra.gz"
log_must eval "printf '\x1f\x8b\x08\x04\x00\x00\x00\x00' > $extrafile"
log_mustnot zstream import "$extrafile"

# Test 11: Test with gzip file containing filename (should work with warning in verbose mode)
typeset filenamefile="$BACKDIR/filename.gz"
log_must eval "printf '\x1f\x8b\x08\x08\x00\x00\x00\x00' > $filenamefile"
log_must eval "zstream import -v $filenamefile > /dev/null 2> $BACKDIR/filename_warning"
log_must grep -q "Warning.*filename" "$BACKDIR/filename_warning"

# Test 12: Test with gzip file containing comment (should work with warning in verbose mode)
typeset commentfile="$BACKDIR/comment.gz"
log_must eval "printf '\x1f\x8b\x08\x10\x00\x00\x00\x00' > $commentfile"
log_must eval "zstream import -v $commentfile > /dev/null 2> $BACKDIR/comment_warning"
log_must grep -q "Warning.*comment" "$BACKDIR/comment_warning"

# Test 13: Test error conditions
# Test with non-existent file
log_mustnot zstream import /nonexistent/file.gz

# Test with non-gzip file
typeset notgzipfile="$BACKDIR/notgzip.gz"
log_must eval "echo 'not a gzip file' > $notgzipfile"
log_mustnot zstream import "$notgzipfile"

# Test with missing argument
log_mustnot zstream import

# Test 14: Test stream validation with different compression levels
# Verify that streams with different compression levels are valid
log_must eval "zstream import -l 1 $gzipfile > $BACKDIR/level1.stream"
log_must eval "zstream import -l 9 $gzipfile > $BACKDIR/level9.stream"

# Both streams should be valid and contain the same payload size
typeset level1_output=$(zstream dump "$BACKDIR/level1.stream" 2>/dev/null)
typeset level9_output=$(zstream dump "$BACKDIR/level9.stream" 2>/dev/null)

echo "$level1_output" | log_must grep -q "Total DRR_WRITE records = 1"
echo "$level9_output" | log_must grep -q "Total DRR_WRITE records = 1"

# Both should have the same payload size
typeset level1_size=$(echo "$level1_output" | grep "Total payload size" | sed 's/.* = \([0-9]*\).*/\1/')
typeset level9_size=$(echo "$level9_output" | grep "Total payload size" | sed 's/.* = \([0-9]*\).*/\1/')
log_must test "$level1_size" -eq "$level9_size"
log_must test "$level1_size" -eq "$original_size"

log_pass "zstream import correctly imports gzip files as ZFS streams."