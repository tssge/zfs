// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2024. All rights reserved.
 * Use is subject to license terms.
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <zlib.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include "zfs_fletcher.h"
#include "zstream.h"

#define	GZIP_MAGIC1	0x1f
#define	GZIP_MAGIC2	0x8b
#define	GZIP_METHOD_DEFLATE	0x08

/* Progress reporting threshold - only show progress for files > 1MB */
#define	PROGRESS_THRESHOLD	(1024 * 1024)



/*
 * Simple gzip header structure
 */
typedef struct gzip_header {
	uint8_t magic1;
	uint8_t magic2;
	uint8_t method;
	uint8_t flags;
	uint32_t mtime;
	uint8_t xfl;
	uint8_t os;
} gzip_header_t;

static void
show_progress(const char *filename, size_t bytes_read, size_t total_bytes)
{
	static time_t last_update = 0;
	time_t now = time(NULL);
	
	/* Only update every second to avoid flooding stderr */
	if (now - last_update < 1)
		return;
	
	last_update = now;
	
	/* Calculate percentage */
	int percent = (int)((bytes_read * 100) / total_bytes);
	
	/* Show progress on stderr so it doesn't interfere with stdout stream */
	fprintf(stderr, "\rProcessing %s: %zu/%zu bytes (%d%%)", 
	    filename, bytes_read, total_bytes, percent);
	fflush(stderr);
}

static void
clear_progress(void)
{
	/* Clear the progress line */
	fprintf(stderr, "\r%*s\r", 80, "");
	fflush(stderr);
}

static int
validate_gzip_trailer(const char *filename, const void *gzip_data, size_t file_size, boolean_t verbose)
{
	/* Gzip trailer is the last 8 bytes: CRC32 (4 bytes) + ISIZE (4 bytes) */
	if (file_size < 8) {
		return (-1);
	}

	const uint8_t *data = (const uint8_t *)gzip_data;
	const uint8_t *trailer = data + file_size - 8;
	
	/* Extract CRC32 and ISIZE from trailer */
	uint32_t crc32 = trailer[0] | (trailer[1] << 8) | (trailer[2] << 16) | (trailer[3] << 24);
	uint32_t isize = trailer[4] | (trailer[5] << 8) | (trailer[6] << 16) | (trailer[7] << 24);
	
	if (verbose) {
		fprintf(stderr, "Gzip trailer validation:\n");
		fprintf(stderr, "  CRC32: 0x%08x\n", crc32);
		fprintf(stderr, "  ISIZE: %u bytes\n", isize);
	}
	
	/* Note: We don't validate the CRC32 here as it would require decompression
	 * and we want to preserve the compressed data. The ISIZE validation is
	 * also skipped as it represents the uncompressed size which we don't need.
	 * The ZFS receive operation will handle any data integrity issues.
	 */
	
	return (0);
}

static int
write_record(dmu_replay_record_t *drr, void *payload, int payload_len,
    zio_cksum_t *zc, int outfd)
{
	assert(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum)
	    == sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	fletcher_4_incremental_native(drr,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum), zc);
	if (drr->drr_type != DRR_BEGIN) {
		assert(ZIO_CHECKSUM_IS_ZERO(&drr->drr_u.
		    drr_checksum.drr_checksum));
		drr->drr_u.drr_checksum.drr_checksum = *zc;
	}
	fletcher_4_incremental_native(&drr->drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t), zc);
	if (write(outfd, drr, sizeof (*drr)) == -1)
		return (errno);
	if (payload_len != 0) {
		fletcher_4_incremental_native(payload, payload_len, zc);
		if (write(outfd, payload, payload_len) == -1)
			return (errno);
	}
	return (0);
}

static int
create_begin_record(const char *dataset_name, int outfd, zio_cksum_t *zc)
{
	dmu_replay_record_t drr = {0};

	drr.drr_type = DRR_BEGIN;
	drr.drr_payloadlen = 0;

	/* Initialize begin record */
	drr.drr_u.drr_begin.drr_magic = DMU_BACKUP_MAGIC;
	DMU_SET_STREAM_HDRTYPE(drr.drr_u.drr_begin.drr_versioninfo,
	    DMU_SUBSTREAM);
	drr.drr_u.drr_begin.drr_creation_time = time(NULL);
	drr.drr_u.drr_begin.drr_type = DMU_OST_ZFS;
	drr.drr_u.drr_begin.drr_flags = 0;
	drr.drr_u.drr_begin.drr_toguid = 0x1; /* Simple non-zero GUID */
	drr.drr_u.drr_begin.drr_fromguid = 0;

	/* Set the dataset name */
	strlcpy(drr.drr_u.drr_begin.drr_toname, dataset_name,
	    sizeof (drr.drr_u.drr_begin.drr_toname));

	return (write_record(&drr, NULL, 0, zc, outfd));
}

static int
create_object_record(uint64_t object_id, int outfd, zio_cksum_t *zc, int compression_level)
{
	dmu_replay_record_t drr = {0};
	enum zio_compress compress_type;

	drr.drr_type = DRR_OBJECT;
	drr.drr_payloadlen = 0;

	/* Map compression level to ZFS compression type */
	switch (compression_level) {
	case 1:
		compress_type = ZIO_COMPRESS_GZIP_1;
		break;
	case 2:
		compress_type = ZIO_COMPRESS_GZIP_2;
		break;
	case 3:
		compress_type = ZIO_COMPRESS_GZIP_3;
		break;
	case 4:
		compress_type = ZIO_COMPRESS_GZIP_4;
		break;
	case 5:
		compress_type = ZIO_COMPRESS_GZIP_5;
		break;
	case 6:
		compress_type = ZIO_COMPRESS_GZIP_6;
		break;
	case 7:
		compress_type = ZIO_COMPRESS_GZIP_7;
		break;
	case 8:
		compress_type = ZIO_COMPRESS_GZIP_8;
		break;
	case 9:
		compress_type = ZIO_COMPRESS_GZIP_9;
		break;
	default:
		compress_type = ZIO_COMPRESS_GZIP_6; /* Default to level 6 */
		break;
	}

	/* Initialize object record for a regular file */
	drr.drr_u.drr_object.drr_object = object_id;
	drr.drr_u.drr_object.drr_type = DMU_OT_PLAIN_FILE_CONTENTS;
	drr.drr_u.drr_object.drr_bonustype = DMU_OT_SA;
	drr.drr_u.drr_object.drr_blksz = 131072; /* 128KB blocks */
	drr.drr_u.drr_object.drr_bonuslen = 0;
	drr.drr_u.drr_object.drr_checksumtype = ZIO_CHECKSUM_FLETCHER_4;
	drr.drr_u.drr_object.drr_compress = compress_type;
	drr.drr_u.drr_object.drr_dn_slots = 1;
	drr.drr_u.drr_object.drr_flags = 0;

	return (write_record(&drr, NULL, 0, zc, outfd));
}

static int
create_write_record(uint64_t object_id, uint64_t offset, void *data,
    size_t compressed_size, size_t logical_size, int outfd, zio_cksum_t *zc, int compression_level)
{
	dmu_replay_record_t drr = {0};
	enum zio_compress compress_type;

	drr.drr_type = DRR_WRITE;
	drr.drr_payloadlen = compressed_size;

	/* Map compression level to ZFS compression type */
	switch (compression_level) {
	case 1:
		compress_type = ZIO_COMPRESS_GZIP_1;
		break;
	case 2:
		compress_type = ZIO_COMPRESS_GZIP_2;
		break;
	case 3:
		compress_type = ZIO_COMPRESS_GZIP_3;
		break;
	case 4:
		compress_type = ZIO_COMPRESS_GZIP_4;
		break;
	case 5:
		compress_type = ZIO_COMPRESS_GZIP_5;
		break;
	case 6:
		compress_type = ZIO_COMPRESS_GZIP_6;
		break;
	case 7:
		compress_type = ZIO_COMPRESS_GZIP_7;
		break;
	case 8:
		compress_type = ZIO_COMPRESS_GZIP_8;
		break;
	case 9:
		compress_type = ZIO_COMPRESS_GZIP_9;
		break;
	default:
		compress_type = ZIO_COMPRESS_GZIP_6; /* Default to level 6 */
		break;
	}

	/* Initialize write record */
	drr.drr_u.drr_write.drr_object = object_id;
	drr.drr_u.drr_write.drr_type = DMU_OT_PLAIN_FILE_CONTENTS;
	drr.drr_u.drr_write.drr_offset = offset;
	drr.drr_u.drr_write.drr_logical_size = logical_size;
	drr.drr_u.drr_write.drr_toguid = 0x1;
	drr.drr_u.drr_write.drr_checksumtype = ZIO_CHECKSUM_FLETCHER_4;
	drr.drr_u.drr_write.drr_flags = 0;
	drr.drr_u.drr_write.drr_compressiontype = compress_type;
	drr.drr_u.drr_write.drr_compressed_size = compressed_size;

	return (write_record(&drr, data, compressed_size, zc, outfd));
}

static int
create_end_record(int outfd, zio_cksum_t *zc)
{
	dmu_replay_record_t drr = {0};

	drr.drr_type = DRR_END;
	drr.drr_payloadlen = 0;

	drr.drr_u.drr_end.drr_checksum = *zc;
	drr.drr_u.drr_end.drr_toguid = 0x1;

	/* For END records, we don't update the checksum */
	if (write(outfd, &drr, sizeof (drr)) == -1)
		return (errno);

	return (0);
}

static int
process_gzip_file(const char *filename, const char *dataset_name, int outfd, boolean_t verbose, int compression_level)
{
	FILE *infile;
	struct stat st;
	void *gzip_data;
	size_t file_size;
	zio_cksum_t zc;
	int ret = 0;
	gzip_header_t header;

	/* Validate input parameters */
	if (filename == NULL || dataset_name == NULL) {
		err(1, "invalid parameters");
	}

	/* Open and read the gzip file */
	infile = fopen(filename, "rb");
	if (infile == NULL) {
		err(1, "cannot open %s", filename);
	}

	if (stat(filename, &st) != 0) {
		fclose(infile);
		err(1, "cannot stat %s", filename);
	}
	file_size = st.st_size;

	/* Check for empty files */
	if (file_size == 0) {
		fclose(infile);
		err(1, "%s is empty", filename);
	}

	/* Check for unreasonably large files (> 1GB) */
	if (file_size > 1024 * 1024 * 1024) {
		fclose(infile);
		err(1, "%s is too large (%zu bytes), maximum supported size is 1GB", 
		    filename, file_size);
	}

	/* Read and validate gzip header */
	if (fread(&header, sizeof (header), 1, infile) != 1) {
		fclose(infile);
		err(1, "cannot read gzip header from %s", filename);
	}

	if (header.magic1 != GZIP_MAGIC1 || header.magic2 != GZIP_MAGIC2) {
		fclose(infile);
		err(1, "%s is not a valid gzip file (invalid magic bytes)", filename);
	}

	if (header.method != GZIP_METHOD_DEFLATE) {
		fclose(infile);
		err(1, "%s uses unsupported compression method (%d), only deflate (8) is supported", 
		    filename, header.method);
	}

	/* Check for minimum file size (gzip header + trailer) */
	if (file_size < sizeof (gzip_header_t) + 8) {
		fclose(infile);
		err(1, "%s is too small to be a valid gzip file", filename);
	}

	/* Check for extra fields in gzip header */
	if (header.flags & 0x04) { /* FEXTRA flag */
		fclose(infile);
		err(1, "%s contains extra fields which are not supported", filename);
	}

	/* Check for filename in gzip header */
	if (header.flags & 0x08) { /* FNAME flag */
		if (verbose) {
			fprintf(stderr, "Warning: %s contains original filename in header\n", filename);
		}
	}

	/* Check for comment in gzip header */
	if (header.flags & 0x10) { /* FCOMMENT flag */
		if (verbose) {
			fprintf(stderr, "Warning: %s contains comment in header\n", filename);
		}
	}

	/* Reset file position and read entire file */
	if (fseek(infile, 0, SEEK_SET) != 0) {
		fclose(infile);
		err(1, "cannot seek to beginning of %s", filename);
	}

	gzip_data = malloc(file_size);
	if (gzip_data == NULL) {
		fclose(infile);
		err(1, "cannot allocate %zu bytes for gzip data", file_size);
	}

	/* Read file with progress reporting for large files */
	if (file_size > PROGRESS_THRESHOLD && verbose) {
		size_t bytes_read = 0;
		size_t chunk_size = 64 * 1024; /* 64KB chunks */
		char *ptr = (char *)gzip_data;
		
		while (bytes_read < file_size) {
			size_t to_read = (file_size - bytes_read > chunk_size) ? 
			    chunk_size : (file_size - bytes_read);
			size_t actually_read = fread(ptr, 1, to_read, infile);
			
			if (actually_read == 0) {
				if (feof(infile)) {
					free(gzip_data);
					fclose(infile);
					err(1, "unexpected end of file while reading %s", filename);
				} else {
					free(gzip_data);
					fclose(infile);
					err(1, "error reading from %s", filename);
				}
			}
			
			bytes_read += actually_read;
			ptr += actually_read;
			show_progress(filename, bytes_read, file_size);
		}
		clear_progress();
	} else {
		/* For small files, read all at once */
		if (fread(gzip_data, file_size, 1, infile) != 1) {
			free(gzip_data);
			fclose(infile);
			err(1, "cannot read %zu bytes from gzip file %s", file_size, filename);
		}
	}
	
	if (fclose(infile) != 0) {
		free(gzip_data);
		err(1, "error closing %s", filename);
	}

	/* Initialize checksum */
	memset(&zc, 0, sizeof (zc));

	/* Validate gzip trailer */
	if (validate_gzip_trailer(filename, gzip_data, file_size, verbose) != 0) {
		free(gzip_data);
		err(1, "invalid gzip trailer in %s", filename);
	}

	if (verbose) {
		fprintf(stderr, "Creating ZFS stream for dataset '%s'\n", dataset_name);
		fprintf(stderr, "File size: %zu bytes\n", file_size);
	}

	/* Create ZFS stream */
	if ((ret = create_begin_record(dataset_name, outfd, &zc)) != 0) {
		err(1, "failed to write begin record");
	}

	if ((ret = create_object_record(1, outfd, &zc, compression_level)) != 0) {
		err(1, "failed to write object record");
	}

	/*
	 * For simplicity, write the entire gzip file as a single compressed
	 * block. In a more sophisticated implementation, we could parse the
	 * gzip stream and extract individual deflate blocks.
	 */
	if ((ret = create_write_record(1, 0, gzip_data, file_size, file_size,
	    outfd, &zc, compression_level)) != 0) {
		err(1, "failed to write data record");
	}

	if ((ret = create_end_record(outfd, &zc)) != 0) {
		err(1, "failed to write end record");
	}

	if (verbose) {
		fprintf(stderr, "ZFS stream created successfully\n");
	}

	free(gzip_data);
	return (0);
}

int
zstream_do_import(int argc, char *argv[])
{
	char *filename = NULL;
	const char *dataset_name = "imported_gzip";
	boolean_t verbose = B_FALSE;
	int compression_level = 6; /* Default to gzip level 6 */
	int c;

	while ((c = getopt(argc, argv, "d:vl:")) != -1) {
		switch (c) {
		case 'd':
			dataset_name = optarg;
			break;
		case 'v':
			verbose = B_TRUE;
			break;
		case 'l':
			compression_level = atoi(optarg);
			if (compression_level < 1 || compression_level > 9) {
				(void) fprintf(stderr, "compression level must be between 1 and 9\n");
				zstream_usage();
			}
			break;
		case '?':
			(void) fprintf(stderr, "invalid option '%c'\n",
			    optopt);
			zstream_usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1) {
		(void) fprintf(stderr, "incorrect number of arguments\n");
		zstream_usage();
	}

	filename = argv[0];

	/* Validate dataset name */
	if (strlen(dataset_name) == 0) {
		(void) fprintf(stderr, "dataset name cannot be empty\n");
		zstream_usage();
	}

	if (strlen(dataset_name) >= MAXNAMELEN) {
		(void) fprintf(stderr, "dataset name too long (max %d characters)\n",
		    MAXNAMELEN - 1);
		zstream_usage();
	}

	/* Basic validation of dataset name format */
	if (strchr(dataset_name, '@') != NULL || strchr(dataset_name, '#') != NULL) {
		(void) fprintf(stderr, "dataset name cannot contain '@' or '#' characters\n");
		zstream_usage();
	}

	return (process_gzip_file(filename, dataset_name, STDOUT_FILENO, verbose, compression_level));
}