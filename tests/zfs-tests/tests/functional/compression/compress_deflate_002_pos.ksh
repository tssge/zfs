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
# Compare deflate compression with other compression algorithms to ensure
# it provides reasonable compression ratios and works correctly.
#
# STRATEGY:
# 1. Create test data that should compress well
# 2. Test with different compression algorithms including deflate
# 3. Compare compression ratios
# 4. Verify data integrity across all algorithms
#

verify_runnable "both"

function cleanup
{
    # Reset compression to default
    log_must zfs set compression=on $TESTPOOL/$TESTFS
}

log_assert "Compare deflate compression with other algorithms"
log_onexit cleanup

# Create test data that should compress well
log_note "Creating test data for compression comparison"
typeset test_data="This is test data for compression comparison. " \
    "It contains repeated patterns that should compress well. " \
    "The deflate algorithm should provide good compression for this type of data. " \
    "This is test data for compression comparison. " \
    "It contains repeated patterns that should compress well. "

# Create a large file with repeated patterns
typeset -i i=0
while (( i < 500 )); do
    echo -n "$test_data" >> $TESTDIR/compression_test_data
    (( i = i + 1 ))
done

# Get uncompressed size
uncompressed_size=$(stat -c%s $TESTDIR/compression_test_data 2>/dev/null || stat -f%z $TESTDIR/compression_test_data 2>/dev/null)
log_note "Uncompressed data size: $uncompressed_size bytes"

# Test different compression algorithms
typeset -a algorithms=('off' 'lzjb' 'lz4' 'gzip' 'deflate')
typeset -A compressed_sizes

for algo in "${algorithms[@]}"; do
    log_note "Testing compression algorithm: $algo"
    
    # Set compression algorithm
    log_must zfs set compression=$algo $TESTPOOL/$TESTFS
    
    # Wait for compression to take effect
    sleep 30
    
    # Create a copy of the test data
    cp $TESTDIR/compression_test_data $TESTDIR/test_${algo}
    
    # Wait for compression to complete
    sleep 30
    
    # Get compressed size
    compressed_size=$(stat -c%s $TESTDIR/test_${algo} 2>/dev/null || stat -f%z $TESTDIR/test_${algo} 2>/dev/null)
    compressed_sizes[$algo]=$compressed_size
    
    # Calculate compression ratio
    if [[ $algo == "off" ]]; then
        ratio=100
    else
        ratio=$(( (compressed_size * 100) / uncompressed_size ))
    fi
    
    log_note "Algorithm $algo: $compressed_size bytes (${ratio}% of original)"
    
    # Verify data integrity
    if ! diff $TESTDIR/compression_test_data $TESTDIR/test_${algo} > /dev/null; then
        log_fail "Data integrity check failed for algorithm $algo"
    fi
done

# Verify deflate compression is working
deflate_size=${compressed_sizes[deflate]}
if [[ $deflate_size -ge $uncompressed_size ]]; then
    log_fail "Deflate compression not working - compressed size >= uncompressed size"
fi

# Verify deflate provides reasonable compression (should be better than no compression)
off_size=${compressed_sizes[off]}
if [[ $deflate_size -ge $off_size ]]; then
    log_fail "Deflate compression not better than no compression"
fi

# Calculate deflate compression ratio
deflate_ratio=$(( (deflate_size * 100) / uncompressed_size ))
log_note "Deflate compression ratio: ${deflate_ratio}% (${deflate_size}/${uncompressed_size} bytes)"

# Deflate should provide reasonable compression (less than 80% of original for this data)
if [[ $deflate_ratio -gt 80 ]]; then
    log_fail "Deflate compression ratio too high: ${deflate_ratio}%"
fi

log_note "Compression comparison results:"
for algo in "${algorithms[@]}"; do
    size=${compressed_sizes[$algo]}
    if [[ $algo == "off" ]]; then
        ratio=100
    else
        ratio=$(( (size * 100) / uncompressed_size ))
    fi
    log_note "  $algo: $size bytes (${ratio}%)"
done

log_pass "Deflate compression comparison test passed"
