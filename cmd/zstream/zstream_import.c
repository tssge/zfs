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

/*
 * Safe version of fread(), exits on error.
 */
static int
sfread(void *buf, size_t size, FILE *fp)
{
	int rv = fread(buf, size, 1, fp);
	if (rv == 0 && ferror(fp)) {
		(void) fprintf(stderr, "Error while reading file: %s\n",
		    strerror(errno));
		exit(1);
	}
	return (rv);
}

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
	DMU_SET_STREAM_HDRTYPE(drr.drr_u.drr_begin.drr_versioninfo, DMU_SUBSTREAM);
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
create_object_record(uint64_t object_id, const char *filename, 
    size_t file_size, int outfd, zio_cksum_t *zc)
{
	dmu_replay_record_t drr = {0};
	
	drr.drr_type = DRR_OBJECT;
	drr.drr_payloadlen = 0;
	
	/* Initialize object record for a regular file */
	drr.drr_u.drr_object.drr_object = object_id;
	drr.drr_u.drr_object.drr_type = DMU_OT_PLAIN_FILE_CONTENTS;
	drr.drr_u.drr_object.drr_bonustype = DMU_OT_SA;
	drr.drr_u.drr_object.drr_blksz = 131072; /* 128KB blocks */
	drr.drr_u.drr_object.drr_bonuslen = 0;
	drr.drr_u.drr_object.drr_checksumtype = ZIO_CHECKSUM_FLETCHER_4;
	drr.drr_u.drr_object.drr_compress = ZIO_COMPRESS_GZIP_6; /* Default gzip level */
	drr.drr_u.drr_object.drr_dn_slots = 1;
	drr.drr_u.drr_object.drr_flags = 0;

	return (write_record(&drr, NULL, 0, zc, outfd));
}

static int
create_write_record(uint64_t object_id, uint64_t offset, void *data,
    size_t compressed_size, size_t logical_size, int outfd, zio_cksum_t *zc)
{
	dmu_replay_record_t drr = {0};
	
	drr.drr_type = DRR_WRITE;
	drr.drr_payloadlen = compressed_size;
	
	/* Initialize write record */
	drr.drr_u.drr_write.drr_object = object_id;
	drr.drr_u.drr_write.drr_type = DMU_OT_PLAIN_FILE_CONTENTS;
	drr.drr_u.drr_write.drr_offset = offset;
	drr.drr_u.drr_write.drr_logical_size = logical_size;
	drr.drr_u.drr_write.drr_toguid = 0x1;
	drr.drr_u.drr_write.drr_checksumtype = ZIO_CHECKSUM_FLETCHER_4;
	drr.drr_u.drr_write.drr_flags = 0;
	drr.drr_u.drr_write.drr_compressiontype = ZIO_COMPRESS_GZIP_6;
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
process_gzip_file(const char *filename, const char *dataset_name, int outfd)
{
	FILE *infile;
	struct stat st;
	void *gzip_data;
	size_t file_size;
	zio_cksum_t zc;
	int ret = 0;
	gzip_header_t header;

	/* Open and read the gzip file */
	infile = fopen(filename, "rb");
	if (infile == NULL) {
		err(1, "cannot open %s", filename);
	}

	if (stat(filename, &st) != 0) {
		err(1, "cannot stat %s", filename);
	}
	file_size = st.st_size;

	/* Read and validate gzip header */
	if (fread(&header, sizeof (header), 1, infile) != 1) {
		err(1, "cannot read gzip header from %s", filename);
	}

	if (header.magic1 != GZIP_MAGIC1 || header.magic2 != GZIP_MAGIC2) {
		err(1, "%s is not a valid gzip file", filename);
	}

	if (header.method != GZIP_METHOD_DEFLATE) {
		err(1, "%s uses unsupported compression method", filename);
	}

	/* Reset file position and read entire file */
	fseek(infile, 0, SEEK_SET);
	
	gzip_data = malloc(file_size);
	if (gzip_data == NULL) {
		err(1, "cannot allocate memory for gzip data");
	}

	if (fread(gzip_data, file_size, 1, infile) != 1) {
		err(1, "cannot read gzip file %s", filename);
	}
	fclose(infile);

	/* Initialize checksum */
	memset(&zc, 0, sizeof (zc));

	/* Create ZFS stream */
	if ((ret = create_begin_record(dataset_name, outfd, &zc)) != 0) {
		err(1, "failed to write begin record");
	}

	if ((ret = create_object_record(1, filename, file_size, outfd, &zc)) != 0) {
		err(1, "failed to write object record");
	}

	/*
	 * For simplicity, write the entire gzip file as a single compressed block.
	 * In a more sophisticated implementation, we could parse the gzip stream
	 * and extract individual deflate blocks.
	 */
	if ((ret = create_write_record(1, 0, gzip_data, file_size, file_size,
	    outfd, &zc)) != 0) {
		err(1, "failed to write data record");
	}

	if ((ret = create_end_record(outfd, &zc)) != 0) {
		err(1, "failed to write end record");
	}

	free(gzip_data);
	return (0);
}

int
zstream_do_import(int argc, char *argv[])
{
	char *filename = NULL;
	char *dataset_name = "imported_gzip";
	int c;

	while ((c = getopt(argc, argv, "d:")) != -1) {
		switch (c) {
		case 'd':
			dataset_name = optarg;
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

	return (process_gzip_file(filename, dataset_name, STDOUT_FILENO));
}