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
 * Copyright (c) 2024 by [Your Name]. All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Deflate compression support for ZFS
 * 
 * This implementation provides raw deflate compression (RFC 1951) which is
 * compatible with ZIP file format. Unlike gzip, deflate does not include
 * headers or checksums, making it suitable for embedding in other formats
 * like ZIP files.
 */

#include <sys/debug.h>
#include <sys/types.h>
#include <sys/qat.h>
#include <sys/zio_compress.h>

#ifdef _KERNEL

#include <sys/zmod.h>
#include <linux/zlib.h>
typedef size_t zlen_t;

#else /* _KERNEL */

#include <zlib.h>
typedef uLongf zlen_t;

#endif

/*
 * Raw deflate compression using zlib's deflate function with raw deflate
 * format (no gzip headers or checksums).
 */
static size_t
zfs_deflate_compress_buf(void *s_start, void *d_start, size_t s_len,
    size_t d_len, int level)
{
	int ret;
	zlen_t dstlen = d_len;
	z_stream stream;
	unsigned char *src = (unsigned char *)s_start;
	unsigned char *dst = (unsigned char *)d_start;

	ASSERT(d_len <= s_len);

	/* check if hardware accelerator can be used */
	if (qat_dc_use_accel(s_len)) {
		ret = qat_compress(QAT_COMPRESS, s_start, s_len, d_start,
		    d_len, &dstlen);
		if (ret == CPA_STATUS_SUCCESS) {
			return ((size_t)dstlen);
		} else if (ret == CPA_STATUS_INCOMPRESSIBLE) {
			if (d_len != s_len)
				return (s_len);

			memcpy(d_start, s_start, s_len);
			return (s_len);
		}
		/* if hardware compression fails, do it again with software */
	}

	/* Initialize zlib stream for raw deflate */
	memset(&stream, 0, sizeof (stream));
	
	/* Use raw deflate format (no gzip headers) */
	ret = deflateInit2(&stream, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
	if (ret != Z_OK) {
		if (d_len != s_len)
			return (s_len);

		memcpy(d_start, s_start, s_len);
		return (s_len);
	}

	stream.next_in = src;
	stream.avail_in = s_len;
	stream.next_out = dst;
	stream.avail_out = d_len;

	/* Compress the data */
	ret = deflate(&stream, Z_FINISH);
	
	deflateEnd(&stream);

	if (ret != Z_STREAM_END) {
		if (d_len != s_len)
			return (s_len);

		memcpy(d_start, s_start, s_len);
		return (s_len);
	}

	return ((size_t)(d_len - stream.avail_out));
}

/*
 * Raw deflate decompression using zlib's inflate function with raw deflate
 * format (no gzip headers or checksums).
 */
static int
zfs_deflate_decompress_buf(void *s_start, void *d_start, size_t s_len,
    size_t d_len, int level)
{
	int ret;
	z_stream stream;
	unsigned char *src = (unsigned char *)s_start;
	unsigned char *dst = (unsigned char *)d_start;

	(void) level;

	ASSERT(d_len >= s_len);

	/* check if hardware accelerator can be used */
	if (qat_dc_use_accel(d_len)) {
		zlen_t dstlen = d_len;
		if (qat_compress(QAT_DECOMPRESS, s_start, s_len,
		    d_start, d_len, &dstlen) == CPA_STATUS_SUCCESS)
			return (0);
		/* if hardware de-compress fail, do it again with software */
	}

	/* Initialize zlib stream for raw deflate */
	memset(&stream, 0, sizeof (stream));
	
	/* Use raw deflate format (no gzip headers) */
	ret = inflateInit2(&stream, -MAX_WBITS);
	if (ret != Z_OK)
		return (-1);

	stream.next_in = src;
	stream.avail_in = s_len;
	stream.next_out = dst;
	stream.avail_out = d_len;

	/* Decompress the data */
	ret = inflate(&stream, Z_FINISH);
	
	inflateEnd(&stream);

	if (ret != Z_STREAM_END)
		return (-1);

	return (0);
}

ZFS_COMPRESS_WRAP_DECL(zfs_deflate_compress)
ZFS_DECOMPRESS_WRAP_DECL(zfs_deflate_decompress)
