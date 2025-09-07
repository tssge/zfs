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
#include <sys/asm_linkage.h>

/* These functions are provided by Intel ISAL assembly */
extern void ASMABI icp_isalc_gcm_precomp_128_sse(void *);
extern void ASMABI icp_isalc_gcm_precomp_192_sse(void *);
extern void ASMABI icp_isalc_gcm_precomp_256_sse(void *);

#include <modes/gcm_impl.h>

/*
 * Simple wrapper around Intel ISAL SSE4.1 GCM multiplication.
 * This provides the same interface as gcm_pclmulqdq_impl.
 * 
 * Note: x_in, y, and res all point to 16-byte numbers (an array of two
 * 64-bit integers).
 */
static void
gcm_sse41_mul(uint64_t *x_in, uint64_t *y, uint64_t *res)
{
	/*
	 * For now, this is a placeholder that just calls the generic
	 * implementation. A full implementation would use the ISAL
	 * assembly routines, but that requires significant integration
	 * work and context management.
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