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

#ifndef	_GCM_IMPL_H
#define	_GCM_IMPL_H

/*
 * GCM function dispatcher.
 */

#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/zfs_context.h>
#include <sys/crypto/common.h>

/*
 * Conditional assembly support for GCM implementations
 */
#if defined(__x86_64) && defined(HAVE_SSE4_1) && defined(HAVE_AES) && defined(HAVE_PCLMULQDQ)
#define	CAN_USE_GCM_ASM
#define	CAN_USE_GCM_ASM_SSE
#endif

#if defined(__x86_64) && defined(HAVE_AVX) && defined(HAVE_AES) && defined(HAVE_PCLMULQDQ)
#define	CAN_USE_GCM_ASM_AVX
#endif

/*
 * Methods used to define GCM implementation
 *
 * @gcm_mul_f Perform carry-less multiplication
 * @gcm_will_work_f Function tests whether implementation will function
 */
typedef void		(*gcm_mul_f)(uint64_t *, uint64_t *, uint64_t *);
typedef boolean_t	(*gcm_will_work_f)(void);

#define	GCM_IMPL_NAME_MAX (16)

/*
 * SIMD implementation types for GCM
 */
typedef enum gcm_simd_impl {
	GSI_NONE = 0,		/* No SIMD implementation */
	GSI_OSSL_AVX,		/* OpenSSL AVX implementation */
	GSI_ISALC_SSE,		/* Intel ISAL SSE implementation */
	GSI_ISALC_FIRST_IMPL = GSI_ISALC_SSE,
	GSI_ISALC_LAST_IMPL = GSI_ISALC_SSE,
	GSI_ISALC_NUM_IMPL = 1
} gcm_simd_impl_t;

typedef struct gcm_impl_ops {
	gcm_mul_f mul;
	gcm_will_work_f is_supported;
	char name[GCM_IMPL_NAME_MAX];
} gcm_impl_ops_t;

extern const gcm_impl_ops_t gcm_generic_impl;
#if defined(__x86_64) && defined(HAVE_PCLMULQDQ)
extern const gcm_impl_ops_t gcm_pclmulqdq_impl;
#endif

/*
 * Initializes fastest implementation
 */
void gcm_impl_init(void);

/*
 * Returns optimal allowed GCM implementation
 */
const struct gcm_impl_ops *gcm_impl_get_ops(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _GCM_IMPL_H */
