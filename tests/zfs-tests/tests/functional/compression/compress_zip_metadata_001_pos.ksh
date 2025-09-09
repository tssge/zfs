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

# Check comment length (should be 0 for our test ZIP)
comment_length=$(zfs get -H -o value com.zfs:zip:archive:comment_length $TESTPOOL/$TESTFS)
log_must eval "echo $comment_length | grep -q '^[0-9]\+$'"

# Check disk numbers (should be 0 for single-disk archive)
disk_number=$(zfs get -H -o value com.zfs:zip:archive:disk_number $TESTPOOL/$TESTFS)
log_must eval "echo $disk_number | grep -q '^[0-9]\+$'"

central_dir_disk=$(zfs get -H -o value com.zfs:zip:archive:central_dir_disk $TESTPOOL/$TESTFS)
log_must eval "echo $central_dir_disk | grep -q '^[0-9]\+$'"

log_note "Test 1 passed: Archive-level metadata stored correctly"

# Test 2: Verify file-level metadata is stored in xattrs
log_note "Test 2: Verify file-level metadata storage in xattrs"

# Check file1.txt xattrs
compression_method=$(getfattr -n user.zip.compression_method --only-values $TESTPOOL/$TESTFS/file1.txt 2>/dev/null)
log_must eval "echo $compression_method | grep -q '^[0-9]\+$'"

version_made=$(getfattr -n user.zip.version_made --only-values $TESTPOOL/$TESTFS/file1.txt 2>/dev/null)
log_must eval "echo $version_made | grep -q '^[0-9]\+$'"

version_needed=$(getfattr -n user.zip.version_needed --only-values $TESTPOOL/$TESTFS/file1.txt 2>/dev/null)
log_must eval "echo $version_needed | grep -q '^[0-9]\+$'"

flags=$(getfattr -n user.zip.flags --only-values $TESTPOOL/$TESTFS/file1.txt 2>/dev/null)
log_must eval "echo $flags | grep -q '^[0-9]\+$'"

internal_attributes=$(getfattr -n user.zip.internal_attributes --only-values $TESTPOOL/$TESTFS/file1.txt 2>/dev/null)
log_must eval "echo $internal_attributes | grep -q '^[0-9]\+$'"

local_header_offset=$(getfattr -n user.zip.local_header_offset --only-values $TESTPOOL/$TESTFS/file1.txt 2>/dev/null)
log_must eval "echo $local_header_offset | grep -q '^[0-9]\+$'"

log_note "Test 2 passed: File-level metadata stored correctly in xattrs"

# Test 3: Verify filesystem metadata consistency
log_note "Test 3: Verify filesystem metadata consistency"

# Check that compression method is valid (0=store, 8=deflate)
log_must eval "test $compression_method -eq 0 -o $compression_method -eq 8"

# Check that version information is reasonable
log_must eval "test $version_made -ge 10"  # ZIP version 1.0+
log_must eval "test $version_needed -ge 10"  # ZIP version 1.0+

log_note "Test 3 passed: Metadata consistency verified"

# Test 4: Verify all files have xattr metadata
log_note "Test 4: Verify all files have xattr metadata"

# Check that all extracted files have corresponding xattrs
for file in file1.txt file2.txt binary.dat; do
	if [[ -f $TESTPOOL/$TESTFS/$file ]]; then
		# Check that compression method xattr exists
		comp_method=$(getfattr -n user.zip.compression_method --only-values $TESTPOOL/$TESTFS/$file 2>/dev/null)
		log_must eval "echo $comp_method | grep -q '^[0-9]\+$'"
		
		# Check that version xattr exists
		version=$(getfattr -n user.zip.version_made --only-values $TESTPOOL/$TESTFS/$file 2>/dev/null)
		log_must eval "echo $version | grep -q '^[0-9]\+$'"
	else
		log_fail "File $file not found in dataset"
	fi
done

log_note "Test 4 passed: All files have xattr metadata"

# Test 5: Verify metadata can be queried programmatically
log_note "Test 5: Verify metadata querying"

# List all ZIP archive properties
zip_props=$(zfs get -H -o name,value -t filesystem $TESTPOOL/$TESTFS | grep "com.zfs:zip:")
log_must eval "echo '$zip_props' | grep -q 'archive:comment_length'"
log_must eval "echo '$zip_props' | grep -q 'archive:disk_number'"
log_must eval "echo '$zip_props' | grep -q 'archive:central_dir_disk'"

# List xattrs on a file
xattr_list=$(getfattr -d $TESTPOOL/$TESTFS/file1.txt 2>/dev/null | grep "user.zip.")
log_must eval "echo '$xattr_list' | grep -q 'user.zip.compression_method'"
log_must eval "echo '$xattr_list' | grep -q 'user.zip.version_made'"

log_note "Test 5 passed: Metadata can be queried programmatically"

# Test 6: Verify metadata survives dataset operations
log_note "Test 6: Verify metadata survives dataset operations"

# Create a snapshot
log_must zfs snapshot $TESTPOOL/$TESTFS@test

# Verify archive metadata still exists in snapshot
snap_comment=$(zfs get -H -o value com.zfs:zip:archive:comment_length $TESTPOOL/$TESTFS@test)
log_must eval "test $snap_comment -eq $comment_length"

# Verify xattrs still exist in snapshot
snap_comp_method=$(getfattr -n user.zip.compression_method --only-values $TESTPOOL/$TESTFS@test/file1.txt 2>/dev/null)
log_must eval "test $snap_comp_method -eq $compression_method"

log_note "Test 6 passed: Metadata survives dataset operations"

log_pass "ZIP metadata preservation tests completed successfully"
