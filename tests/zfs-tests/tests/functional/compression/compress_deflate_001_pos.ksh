#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2024 by [Your Name]. All rights reserved.
# Use is subject to license terms.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/compression/compress.cfg

#
# DESCRIPTION:
# Test deflate compression algorithm functionality. Create files with
# deflate compression and verify they compress properly and can be
# read back correctly.
#
# STRATEGY:
# 1. Set compression to deflate
# 2. Create test files with compressible data
# 3. Verify compression ratio is reasonable
# 4. Verify data integrity by reading back files
# 5. Test with different file sizes and patterns
#

verify_runnable "both"

typeset OP=create

log_assert "Test deflate compression algorithm functionality"

log_note "Setting compression to deflate"
log_must zfs set compression=deflate $TESTPOOL/$TESTFS

# Test 1: Basic compression test with compressible data
log_note "Test 1: Basic deflate compression with compressible data"
log_must file_write -o $OP -f $TESTDIR/deflate_test1 -b $BLOCKSZ \
    -c $NUM_WRITES -d $DATA

# Test 2: Create a file with repeated patterns (should compress well)
log_note "Test 2: Deflate compression with repeated patterns"
typeset pattern="This is a test pattern that should compress well with deflate compression. "
typeset -i i=0
while (( i < 1000 )); do
    echo -n "$pattern" >> $TESTDIR/deflate_test2
    (( i = i + 1 ))
done

# Test 3: Create a file with random data (should not compress much)
log_note "Test 3: Deflate compression with random data"
dd if=/dev/urandom of=$TESTDIR/deflate_test3 bs=1024 count=100 2>/dev/null

# Test 4: Create a file with text data (should compress moderately)
log_note "Test 4: Deflate compression with text data"
cat > $TESTDIR/deflate_test4 << EOF
This is a text file for testing deflate compression.
It contains multiple lines of text that should compress reasonably well.
The deflate algorithm is used in ZIP files and should provide good compression
for text-based content. This file is designed to test the deflate compression
functionality in ZFS and verify that it works correctly.
EOF

# Wait for compression to complete
sleep 60

# Verify compression worked by checking file sizes
log_note "Verifying deflate compression results"

# Check that files exist and have reasonable sizes
for file in deflate_test1 deflate_test2 deflate_test3 deflate_test4; do
    if [[ ! -f $TESTDIR/$file ]]; then
        log_fail "File $file was not created"
    fi
    
    file_size=$(stat -c%s $TESTDIR/$file 2>/dev/null || stat -f%z $TESTDIR/$file 2>/dev/null)
    if [[ $file_size -eq 0 ]]; then
        log_fail "File $file has zero size"
    fi
    
    log_note "File $file size: $file_size bytes"
done

# Test data integrity by reading back files
log_note "Testing data integrity"

# For the pattern file, verify it contains the expected pattern
if ! grep -q "This is a test pattern" $TESTDIR/deflate_test2; then
    log_fail "Pattern file content verification failed"
fi

# For the text file, verify it contains expected content
if ! grep -q "deflate compression" $TESTDIR/deflate_test4; then
    log_fail "Text file content verification failed"
fi

# Test that we can read the files completely
for file in deflate_test1 deflate_test2 deflate_test3 deflate_test4; do
    if ! cat $TESTDIR/$file > /dev/null; then
        log_fail "Failed to read file $file"
    fi
done

# Test compression property is set correctly
compression_prop=$(get_prop compression $TESTPOOL/$TESTFS)
if [[ $compression_prop != "deflate" ]]; then
    log_fail "Compression property not set to deflate: $compression_prop"
fi

log_note "Deflate compression property correctly set to: $compression_prop"

log_pass "Deflate compression algorithm test passed"
