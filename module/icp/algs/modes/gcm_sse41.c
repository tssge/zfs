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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 */

#if defined(__x86_64) && defined(HAVE_SSE4_1) && defined(HAVE_AES) && \
    defined(HAVE_PCLMULQDQ)

#include <sys/types.h>
#include <sys/simd.h>

#include <modes/gcm_impl.h>

/*
 * Simple SSE4.1 GCM implementation.
 * 
 * This implementation leverages Intel ISAL assembly routines for optimized
 * GCM operations using SSE4.1 instructions. For now, this is a placeholder
 * that provides the same interface as other GCM implementations.
 * 
 * Note: x_in, y, and res all point to 16-byte numbers (an array of two
 * 64-bit integers).
 */
static void
gcm_sse41_mul(uint64_t *x_in, uint64_t *y, uint64_t *res)
{
	/*
	 * Placeholder implementation that calls the generic version.
	 * A full implementation would use Intel ISAL SSE4.1 assembly
	 * routines for optimized GCM multiplication.
	 */
	extern void gcm_generic_mul(uint64_t *, uint64_t *, uint64_t *);
	
	kfpu_begin();
	gcm_generic_mul(x_in, y, res);
	kfpu_end();
}

static boolean_t
gcm_sse41_will_work(void)
{
	return (kfpu_allowed() && zfs_sse4_1_available() && 
	        zfs_aes_available() && zfs_pclmulqdq_available());
}

const gcm_impl_ops_t gcm_sse41_impl = {
	.mul = &gcm_sse41_mul,
	.is_supported = &gcm_sse41_will_work,
	.name = "sse41"
};

#endif /* defined(__x86_64) && defined(HAVE_SSE4_1) && defined(HAVE_AES) && 
          defined(HAVE_PCLMULQDQ) */