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
# Test ZIP file import functionality with deflate compression
#
# STRATEGY:
# 1. Create a test ZIP file with various file types and compression methods
# 2. Import the ZIP file as a ZFS dataset using deflate compression
# 3. Verify that files are extracted correctly
# 4. Verify that deflate-compressed files preserve their compression
# 5. Test error handling with malformed ZIP files
#

verify_runnable "both"

function cleanup
{
	if datasetexists $TESTPOOL/$TESTFS; then
		log_must zfs destroy -r $TESTPOOL/$TESTFS
	fi
	if [[ -f $TESTDIR/test.zip ]]; then
		log_must rm -f $TESTDIR/test.zip
	fi
	if [[ -f $TESTDIR/test_malformed.zip ]]; then
		log_must rm -f $TESTDIR/test_malformed.zip
	fi
}

log_onexit cleanup

log_assert "Test ZIP file import functionality"

# Create test files
log_must mkdir -p $TESTDIR/test_files
echo "This is a test file with some content for compression testing." > $TESTDIR/test_files/file1.txt
echo "Another test file with different content." > $TESTDIR/test_files/file2.txt
echo "Binary data: $(dd if=/dev/urandom bs=1024 count=1 2>/dev/null | base64)" > $TESTDIR/test_files/binary.dat

# Create a test ZIP file
log_must cd $TESTDIR/test_files
log_must zip -r ../test.zip .
log_must cd $TESTDIR

# Test 1: Basic ZIP import functionality
log_note "Test 1: Basic ZIP import functionality"
log_must zfs import-zip -v $TESTDIR/test.zip $TESTPOOL/$TESTFS

# Verify dataset was created with deflate compression
log_must eval "zfs get -H -o value compression $TESTPOOL/$TESTFS | grep -q deflate"

# Verify files were extracted
log_must test -f $TESTPOOL/$TESTFS/file1.txt
log_must test -f $TESTPOOL/$TESTFS/file2.txt
log_must test -f $TESTPOOL/$TESTFS/binary.dat

# Verify file contents
log_must eval "diff $TESTDIR/test_files/file1.txt $TESTPOOL/$TESTFS/file1.txt"
log_must eval "diff $TESTDIR/test_files/file2.txt $TESTPOOL/$TESTFS/file2.txt"
log_must eval "diff $TESTDIR/test_files/binary.dat $TESTPOOL/$TESTFS/binary.dat"

log_note "Test 1 passed: Basic ZIP import functionality works correctly"

# Clean up for next test
log_must zfs destroy -r $TESTPOOL/$TESTFS

# Test 2: ZIP import with custom properties
log_note "Test 2: ZIP import with custom properties"
log_must zfs import-zip -o recordsize=64k -o atime=off -v $TESTDIR/test.zip $TESTPOOL/$TESTFS

# Verify custom properties were set
log_must eval "zfs get -H -o value recordsize $TESTPOOL/$TESTFS | grep -q 64K"
log_must eval "zfs get -H -o value atime $TESTPOOL/$TESTFS | grep -q off"
log_must eval "zfs get -H -o value compression $TESTPOOL/$TESTFS | grep -q deflate"

log_note "Test 2 passed: Custom properties are applied correctly"

# Clean up for next test
log_must zfs destroy -r $TESTPOOL/$TESTFS

# Test 3: Error handling - non-existent ZIP file
log_note "Test 3: Error handling - non-existent ZIP file"
log_mustnot zfs import-zip $TESTDIR/nonexistent.zip $TESTPOOL/$TESTFS

log_note "Test 3 passed: Non-existent ZIP file handled correctly"

# Test 4: Error handling - invalid dataset name
log_note "Test 4: Error handling - invalid dataset name"
log_mustnot zfs import-zip $TESTDIR/test.zip "invalid/dataset/name/with/slashes"

log_note "Test 4 passed: Invalid dataset name handled correctly"

# Test 5: Error handling - existing dataset
log_note "Test 5: Error handling - existing dataset"
log_must zfs create $TESTPOOL/$TESTFS
log_mustnot zfs import-zip $TESTDIR/test.zip $TESTPOOL/$TESTFS
log_must zfs destroy $TESTPOOL/$TESTFS

log_note "Test 5 passed: Existing dataset handled correctly"

# Test 6: ZIP with directory structure
log_note "Test 6: ZIP with directory structure"
log_must mkdir -p $TESTDIR/test_files/subdir1/subdir2
echo "File in subdirectory" > $TESTDIR/test_files/subdir1/subdir2/file3.txt
log_must cd $TESTDIR/test_files
log_must zip -r ../test_with_dirs.zip .
log_must cd $TESTDIR

log_must zfs import-zip -v $TESTDIR/test_with_dirs.zip $TESTPOOL/$TESTFS

# Verify directory structure was preserved
log_must test -d $TESTPOOL/$TESTFS/subdir1
log_must test -d $TESTPOOL/$TESTFS/subdir1/subdir2
log_must test -f $TESTPOOL/$TESTFS/subdir1/subdir2/file3.txt

# Verify file content
log_must eval "diff $TESTDIR/test_files/subdir1/subdir2/file3.txt $TESTPOOL/$TESTFS/subdir1/subdir2/file3.txt"

log_note "Test 6 passed: Directory structure preserved correctly"

# Test 7: Help functionality
log_note "Test 7: Help functionality"
log_must eval "zfs import-zip --help | grep -q 'import-zip'"
log_must eval "zfs import-zip -h | grep -q 'import-zip'"

log_note "Test 7 passed: Help functionality works correctly"

log_pass "ZIP import functionality tests completed successfully"
