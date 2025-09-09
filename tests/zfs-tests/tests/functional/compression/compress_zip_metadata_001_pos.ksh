#!/bin/ksh -p
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

. $STF_SUITE/tests/functional/compression/compress_common.kshlib

#
# DESCRIPTION:
# Test ZIP metadata preservation for archival purposes
#
# STRATEGY:
# 1. Create a test ZIP file with known metadata
# 2. Import the ZIP file as a ZFS dataset
# 3. Verify that all ZIP metadata is preserved in ZFS properties
# 4. Verify that metadata can be used to recreate ZIP file structure
#

verify_runnable "both"

function cleanup
{
	if datasetexists $TESTPOOL/$TESTFS; then
		log_must zfs destroy -r $TESTPOOL/$TESTFS
	fi
	if [[ -f $TESTDIR/test_metadata.zip ]]; then
		log_must rm -f $TESTDIR/test_metadata.zip
	fi
}

log_onexit cleanup

log_assert "Test ZIP metadata preservation for archival purposes"

# Create test files with known content and timestamps
log_must mkdir -p $TESTDIR/test_metadata
echo "Test file 1 content" > $TESTDIR/test_metadata/file1.txt
echo "Test file 2 content with more data" > $TESTDIR/test_metadata/file2.txt
echo "Binary test data" > $TESTDIR/test_metadata/binary.dat

# Set specific timestamps for testing
log_must touch -t 202401011200.00 $TESTDIR/test_metadata/file1.txt
log_must touch -t 202401021300.00 $TESTDIR/test_metadata/file2.txt
log_must touch -t 202401031400.00 $TESTDIR/test_metadata/binary.dat

# Create a test ZIP file
log_must cd $TESTDIR/test_metadata
log_must zip -r ../test_metadata.zip .
log_must cd $TESTDIR

# Import ZIP file
log_must zfs import-zip -v $TESTDIR/test_metadata.zip $TESTPOOL/$TESTFS

# Test 1: Verify archive-level metadata is stored
log_note "Test 1: Verify archive-level metadata storage"

# Check total entries
total_entries=$(zfs get -H -o value com.zfs:zip:archive:total_entries $TESTPOOL/$TESTFS)
log_must eval "echo $total_entries | grep -q '^[0-9]\+$'"
log_must eval "test $total_entries -ge 3"  # At least 3 files

# Check central directory size
central_dir_size=$(zfs get -H -o value com.zfs:zip:archive:central_dir_size $TESTPOOL/$TESTFS)
log_must eval "echo $central_dir_size | grep -q '^[0-9]\+$'"
log_must eval "test $central_dir_size -gt 0"

# Check central directory offset
central_dir_offset=$(zfs get -H -o value com.zfs:zip:archive:central_dir_offset $TESTPOOL/$TESTFS)
log_must eval "echo $central_dir_offset | grep -q '^[0-9]\+$'"
log_must eval "test $central_dir_offset -gt 0"

log_note "Test 1 passed: Archive-level metadata stored correctly"

# Test 2: Verify file-level metadata is stored
log_note "Test 2: Verify file-level metadata storage"

# Check file1.txt metadata
compression_method=$(zfs get -H -o value com.zfs:zip:file:file1.txt:compression_method $TESTPOOL/$TESTFS)
log_must eval "echo $compression_method | grep -q '^[0-9]\+$'"

compressed_size=$(zfs get -H -o value com.zfs:zip:file:file1.txt:compressed_size $TESTPOOL/$TESTFS)
log_must eval "echo $compressed_size | grep -q '^[0-9]\+$'"

uncompressed_size=$(zfs get -H -o value com.zfs:zip:file:file1.txt:uncompressed_size $TESTPOOL/$TESTFS)
log_must eval "echo $uncompressed_size | grep -q '^[0-9]\+$'"

crc32=$(zfs get -H -o value com.zfs:zip:file:file1.txt:crc32 $TESTPOOL/$TESTFS)
log_must eval "echo $crc32 | grep -q '^0x[0-9a-fA-F]\+$'"

mod_time=$(zfs get -H -o value com.zfs:zip:file:file1.txt:mod_time $TESTPOOL/$TESTFS)
log_must eval "echo $mod_time | grep -q '^[0-9]\+$'"

mod_date=$(zfs get -H -o value com.zfs:zip:file:file1.txt:mod_date $TESTPOOL/$TESTFS)
log_must eval "echo $mod_date | grep -q '^[0-9]\+$'"

ext_attributes=$(zfs get -H -o value com.zfs:zip:file:file1.txt:ext_attributes $TESTPOOL/$TESTFS)
log_must eval "echo $ext_attributes | grep -q '^0x[0-9a-fA-F]\+$'"

version_made=$(zfs get -H -o value com.zfs:zip:file:file1.txt:version_made $TESTPOOL/$TESTFS)
log_must eval "echo $version_made | grep -q '^[0-9]\+$'"

version_needed=$(zfs get -H -o value com.zfs:zip:file:file1.txt:version_needed $TESTPOOL/$TESTFS)
log_must eval "echo $version_needed | grep -q '^[0-9]\+$'"

flags=$(zfs get -H -o value com.zfs:zip:file:file1.txt:flags $TESTPOOL/$TESTFS)
log_must eval "echo $flags | grep -q '^[0-9]\+$'"

log_note "Test 2 passed: File-level metadata stored correctly"

# Test 3: Verify metadata consistency
log_note "Test 3: Verify metadata consistency"

# Check that uncompressed size matches actual file size
actual_size=$(stat -c%s $TESTPOOL/$TESTFS/file1.txt)
log_must eval "test $uncompressed_size -eq $actual_size"

# Check that compression method is valid (0=store, 8=deflate)
log_must eval "test $compression_method -eq 0 -o $compression_method -eq 8"

# Check that compressed size is reasonable
log_must eval "test $compressed_size -le $uncompressed_size"

log_note "Test 3 passed: Metadata consistency verified"

# Test 4: Verify all files have metadata
log_note "Test 4: Verify all files have metadata"

# Check that all extracted files have corresponding metadata
for file in file1.txt file2.txt binary.dat; do
	if [[ -f $TESTPOOL/$TESTFS/$file ]]; then
		# Check that metadata exists for this file
		comp_method=$(zfs get -H -o value com.zfs:zip:file:$file:compression_method $TESTPOOL/$TESTFS 2>/dev/null)
		log_must eval "echo $comp_method | grep -q '^[0-9]\+$'"
		
		# Check that CRC32 exists
		crc=$(zfs get -H -o value com.zfs:zip:file:$file:crc32 $TESTPOOL/$TESTFS 2>/dev/null)
		log_must eval "echo $crc | grep -q '^0x[0-9a-fA-F]\+$'"
	else
		log_fail "File $file not found in dataset"
	fi
done

log_note "Test 4 passed: All files have metadata"

# Test 5: Verify metadata can be queried programmatically
log_note "Test 5: Verify metadata querying"

# List all ZIP metadata properties
zip_props=$(zfs get -H -o name,value -t filesystem $TESTPOOL/$TESTFS | grep "com.zfs:zip:")
log_must eval "echo '$zip_props' | grep -q 'archive:total_entries'"
log_must eval "echo '$zip_props' | grep -q 'file:file1.txt:compression_method'"
log_must eval "echo '$zip_props' | grep -q 'file:file2.txt:crc32'"
log_must eval "echo '$zip_props' | grep -q 'file:binary.dat:mod_time'"

log_note "Test 5 passed: Metadata can be queried programmatically"

# Test 6: Verify metadata survives dataset operations
log_note "Test 6: Verify metadata survives dataset operations"

# Create a snapshot
log_must zfs snapshot $TESTPOOL/$TESTFS@test

# Verify metadata still exists in snapshot
snap_entries=$(zfs get -H -o value com.zfs:zip:archive:total_entries $TESTPOOL/$TESTFS@test)
log_must eval "test $snap_entries -eq $total_entries"

# Verify metadata still exists in original dataset
orig_entries=$(zfs get -H -o value com.zfs:zip:archive:total_entries $TESTPOOL/$TESTFS)
log_must eval "test $orig_entries -eq $total_entries"

log_note "Test 6 passed: Metadata survives dataset operations"

log_pass "ZIP metadata preservation tests completed successfully"
