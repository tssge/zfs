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
# Verify zstream import gzip validation and error handling
#
# Strategy:
# 1. Test various invalid gzip files
# 2. Test gzip header validation
# 3. Test trailer validation
# 4. Test edge cases and error conditions
# 5. Test verbose warnings for gzip flags
#

verify_runnable "both"

log_assert "Verify zstream import gzip validation and error handling."
log_onexit cleanup_pool "$POOL2"

# Create a valid gzip file for comparison
typeset validfile="$BACKDIR/valid.txt"
typeset validgzip="$BACKDIR/valid.txt.gz"
log_must eval "echo 'This is valid test data' > $validfile"
log_must gzip -c "$validfile" > "$validgzip"

# Test 1: Test empty file
typeset emptyfile="$BACKDIR/empty.gz"
log_must eval "touch $emptyfile"
log_mustnot zstream import "$emptyfile"

# Test 2: Test file too small to be gzip
typeset smallfile="$BACKDIR/small.gz"
log_must eval "echo 'x' > $smallfile"
log_mustnot zstream import "$smallfile"

# Test 3: Test file with invalid magic bytes
typeset badmagic="$BACKDIR/badmagic.gz"
log_must eval "printf '\x1f\x8c\x08\x00\x00\x00\x00\x00' > $badmagic"
log_mustnot zstream import "$badmagic"

# Test 4: Test file with unsupported compression method
typeset badmethod="$BACKDIR/badmethod.gz"
log_must eval "printf '\x1f\x8b\x09\x00\x00\x00\x00\x00' > $badmethod"
log_mustnot zstream import "$badmethod"

# Test 5: Test file with extra fields (FEXTRA flag)
typeset extrafile="$BACKDIR/extra.gz"
log_must eval "printf '\x1f\x8b\x08\x04\x00\x00\x00\x00' > $extrafile"
log_mustnot zstream import "$extrafile"

# Test 6: Test file with filename flag (should work with warning)
typeset filenamefile="$BACKDIR/filename.gz"
log_must eval "printf '\x1f\x8b\x08\x08\x00\x00\x00\x00' > $filenamefile"
log_must eval "zstream import -v $filenamefile > /dev/null 2> $BACKDIR/filename_warning"
log_must grep -q "Warning.*filename" "$BACKDIR/filename_warning"

# Test 7: Test file with comment flag (should work with warning)
typeset commentfile="$BACKDIR/comment.gz"
log_must eval "printf '\x1f\x8b\x08\x10\x00\x00\x00\x00' > $commentfile"
log_must eval "zstream import -v $commentfile > /dev/null 2> $BACKDIR/comment_warning"
log_must grep -q "Warning.*comment" "$BACKDIR/comment_warning"

# Test 8: Test file with both filename and comment flags
typeset bothfile="$BACKDIR/both.gz"
log_must eval "printf '\x1f\x8b\x08\x18\x00\x00\x00\x00' > $bothfile"
log_must eval "zstream import -v $bothfile > /dev/null 2> $BACKDIR/both_warning"
log_must grep -q "Warning.*filename" "$BACKDIR/both_warning"
log_must grep -q "Warning.*comment" "$BACKDIR/both_warning"

# Test 9: Test file with CRC flag (should work)
typeset crcfile="$BACKDIR/crc.gz"
log_must eval "printf '\x1f\x8b\x08\x02\x00\x00\x00\x00' > $crcfile"
log_must eval "zstream import $crcfile > /dev/null"

# Test 10: Test file with all supported flags except FEXTRA
typeset allflags="$BACKDIR/allflags.gz"
log_must eval "printf '\x1f\x8b\x08\x1a\x00\x00\x00\x00' > $allflags"
log_must eval "zstream import -v $allflags > /dev/null 2> $BACKDIR/allflags_warning"
log_must grep -q "Warning.*filename" "$BACKDIR/allflags_warning"
log_must grep -q "Warning.*comment" "$BACKDIR/allflags_warning"

# Test 11: Test gzip trailer validation
# Create a minimal gzip file with proper header and trailer
typeset minimalfile="$BACKDIR/minimal.gz"
# Gzip header (10 bytes) + minimal deflate data + trailer (8 bytes)
log_must eval "printf '\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\xff\xff\x00\x00\x00\x00\x00\x00' > $minimalfile"
log_must eval "zstream import -v $minimalfile > /dev/null 2> $BACKDIR/minimal_output"
log_must grep -q "Gzip trailer validation" "$BACKDIR/minimal_output"

# Test 12: Test file with invalid trailer (too short)
typeset shorttrailer="$BACKDIR/shorttrailer.gz"
log_must eval "printf '\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\xff\xff' > $shorttrailer"
log_mustnot zstream import "$shorttrailer"

# Test 13: Test dataset name validation
log_mustnot zstream import -d "" "$validgzip"
log_mustnot zstream import -d "invalid@name" "$validgzip"
log_mustnot zstream import -d "invalid#name" "$validgzip"

# Test 14: Test very long dataset name
typeset longname=$(printf "a%.0s" {1..300})
log_mustnot zstream import -d "$longname" "$validgzip"

# Test 15: Test compression level validation
log_mustnot zstream import -l 0 "$validgzip"
log_mustnot zstream import -l 10 "$validgzip"
log_mustnot zstream import -l invalid "$validgzip"

# Test 16: Test valid compression levels
log_must eval "zstream import -l 1 $validgzip > /dev/null"
log_must eval "zstream import -l 5 $validgzip > /dev/null"
log_must eval "zstream import -l 9 $validgzip > /dev/null"

# Test 17: Test verbose output for valid file
log_must eval "zstream import -v $validgzip > /dev/null 2> $BACKDIR/valid_verbose"
log_must grep -q "Creating ZFS stream" "$BACKDIR/valid_verbose"
log_must grep -q "File size:" "$BACKDIR/valid_verbose"
log_must grep -q "Gzip trailer validation" "$BACKDIR/valid_verbose"
log_must grep -q "ZFS stream created successfully" "$BACKDIR/valid_verbose"

# Test 18: Test that verbose mode shows CRC32 and ISIZE
log_must grep -q "CRC32:" "$BACKDIR/valid_verbose"
log_must grep -q "ISIZE:" "$BACKDIR/valid_verbose"

# Test 19: Test error messages are informative
log_must eval "zstream import /nonexistent/file.gz 2> $BACKDIR/error_msg"
log_must grep -q "cannot open" "$BACKDIR/error_msg"

log_must eval "zstream import $emptyfile 2> $BACKDIR/empty_error"
log_must grep -q "is empty" "$BACKDIR/empty_error"

log_must eval "zstream import $badmagic 2> $BACKDIR/magic_error"
log_must grep -q "invalid magic bytes" "$BACKDIR/magic_error"

log_must eval "zstream import $badmethod 2> $BACKDIR/method_error"
log_must grep -q "unsupported compression method" "$BACKDIR/method_error"

log_must eval "zstream import $extrafile 2> $BACKDIR/extra_error"
log_must grep -q "extra fields which are not supported" "$BACKDIR/extra_error"

log_pass "zstream import gzip validation and error handling works correctly."
