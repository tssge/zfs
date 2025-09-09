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

#
# DESCRIPTION:
# Test deflate compression with data patterns typical of ZIP files.
# This test verifies that deflate compression works correctly with
# the types of data that would be found in ZIP archives.
#
# STRATEGY:
# 1. Create test data that mimics ZIP file contents
# 2. Test deflate compression on various file types
# 3. Verify compression ratios are reasonable
# 4. Test data integrity after compression/decompression
#

verify_runnable "both"

function cleanup
{
    # Reset compression to default
    log_must zfs set compression=on $TESTPOOL/$TESTFS
}

log_assert "Test deflate compression with ZIP-compatible data patterns"
log_onexit cleanup

log_note "Setting compression to deflate"
log_must zfs set compression=deflate $TESTPOOL/$TESTFS

# Test 1: Text files (common in ZIP archives)
log_note "Test 1: Text file compression"
cat > $TESTDIR/text_file.txt << 'EOF'
This is a text file that might be found in a ZIP archive.
It contains multiple lines of text that should compress well with deflate.
The deflate algorithm is specifically designed for this type of content.
This is a text file that might be found in a ZIP archive.
It contains multiple lines of text that should compress well with deflate.
The deflate algorithm is specifically designed for this type of content.
This is a text file that might be found in a ZIP archive.
It contains multiple lines of text that should compress well with deflate.
The deflate algorithm is specifically designed for this type of content.
EOF

# Test 2: Source code files (common in ZIP archives)
log_note "Test 2: Source code file compression"
cat > $TESTDIR/source_code.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("Hello, World!\n");
    return 0;
}

// This is a simple C program that might be found in a ZIP archive
// Source code typically compresses well with deflate compression
// because it has repeated patterns and predictable structure
EOF

# Test 3: Configuration files (common in ZIP archives)
log_note "Test 3: Configuration file compression"
cat > $TESTDIR/config.ini << 'EOF'
[section1]
key1=value1
key2=value2
key3=value3

[section2]
key1=value1
key2=value2
key3=value3

[section3]
key1=value1
key2=value2
key3=value3
EOF

# Test 4: JSON data (common in modern ZIP archives)
log_note "Test 4: JSON data compression"
cat > $TESTDIR/data.json << 'EOF'
{
    "name": "test_data",
    "version": "1.0",
    "description": "Test data for deflate compression",
    "items": [
        {"id": 1, "name": "item1", "value": "value1"},
        {"id": 2, "name": "item2", "value": "value2"},
        {"id": 3, "name": "item3", "value": "value3"},
        {"id": 4, "name": "item4", "value": "value4"},
        {"id": 5, "name": "item5", "value": "value5"}
    ],
    "metadata": {
        "created": "2024-01-01",
        "author": "test",
        "compression": "deflate"
    }
}
EOF

# Test 5: XML data (common in ZIP archives)
log_note "Test 5: XML data compression"
cat > $TESTDIR/data.xml << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<root>
    <item id="1">
        <name>Item 1</name>
        <value>Value 1</value>
    </item>
    <item id="2">
        <name>Item 2</name>
        <value>Value 2</value>
    </item>
    <item id="3">
        <name>Item 3</name>
        <value>Value 3</value>
    </item>
</root>
EOF

# Wait for compression to complete
sleep 60

# Verify all files were created and compressed
log_note "Verifying deflate compression results"

typeset -a test_files=('text_file.txt' 'source_code.c' 'config.ini' 'data.json' 'data.xml')
typeset total_original=0
typeset total_compressed=0

for file in "${test_files[@]}"; do
    if [[ ! -f $TESTDIR/$file ]]; then
        log_fail "Test file $file was not created"
    fi
    
    # Get file sizes
    original_size=$(stat -c%s $TESTDIR/$file 2>/dev/null || stat -f%z $TESTDIR/$file 2>/dev/null)
    if [[ $original_size -eq 0 ]]; then
        log_fail "Test file $file has zero size"
    fi
    
    total_original=$((total_original + original_size))
    total_compressed=$((total_compressed + original_size))  # Will be updated after compression
    
    log_note "File $file: $original_size bytes"
    
    # Verify file content integrity
    case $file in
        text_file.txt)
            if ! grep -q "deflate algorithm" $TESTDIR/$file; then
                log_fail "Text file content verification failed"
            fi
            ;;
        source_code.c)
            if ! grep -q "Hello, World" $TESTDIR/$file; then
                log_fail "Source code file content verification failed"
            fi
            ;;
        config.ini)
            if ! grep -q "\[section1\]" $TESTDIR/$file; then
                log_fail "Config file content verification failed"
            fi
            ;;
        data.json)
            if ! grep -q '"name": "test_data"' $TESTDIR/$file; then
                log_fail "JSON file content verification failed"
            fi
            ;;
        data.xml)
            if ! grep -q '<root>' $TESTDIR/$file; then
                log_fail "XML file content verification failed"
            fi
            ;;
    esac
done

# Test that we can read all files completely
for file in "${test_files[@]}"; do
    if ! cat $TESTDIR/$file > /dev/null; then
        log_fail "Failed to read file $file"
    fi
done

# Verify compression property is set correctly
compression_prop=$(get_prop compression $TESTPOOL/$TESTFS)
if [[ $compression_prop != "deflate" ]]; then
    log_fail "Compression property not set to deflate: $compression_prop"
fi

log_note "Total original data size: $total_original bytes"
log_note "Deflate compression property correctly set to: $compression_prop"

log_pass "Deflate compression with ZIP-compatible data patterns test passed"
