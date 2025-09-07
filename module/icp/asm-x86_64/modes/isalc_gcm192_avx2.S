/*
 * SPDX-License-Identifier: CDDL-1.0
 * 
 * Placeholder AVX2 implementation for AES-GCM 192-bit keys
 * This is a minimal stub that falls back to SSE implementation
 * TODO: Implement optimized AVX2 routines
 */

#if defined(__x86_64__) && defined(HAVE_AVX2) && defined(HAVE_AES) && \
    defined(HAVE_PCLMULQDQ)

.text

.globl icp_isalc_gcm_precomp_192_avx2
.type icp_isalc_gcm_precomp_192_avx2, @function
icp_isalc_gcm_precomp_192_avx2:
	jmp icp_isalc_gcm_precomp_192_sse

.globl icp_isalc_gcm_init_192_avx2
.type icp_isalc_gcm_init_192_avx2, @function
icp_isalc_gcm_init_192_avx2:
	jmp icp_isalc_gcm_init_192_sse

.globl icp_isalc_gcm_enc_192_update_avx2
.type icp_isalc_gcm_enc_192_update_avx2, @function
icp_isalc_gcm_enc_192_update_avx2:
	jmp icp_isalc_gcm_enc_192_update_sse

.globl icp_isalc_gcm_dec_192_update_avx2
.type icp_isalc_gcm_dec_192_update_avx2, @function
icp_isalc_gcm_dec_192_update_avx2:
	jmp icp_isalc_gcm_dec_192_update_sse

.globl icp_isalc_gcm_enc_192_finalize_avx2
.type icp_isalc_gcm_enc_192_finalize_avx2, @function
icp_isalc_gcm_enc_192_finalize_avx2:
	jmp icp_isalc_gcm_enc_192_finalize_sse

.globl icp_isalc_gcm_dec_192_finalize_avx2
.type icp_isalc_gcm_dec_192_finalize_avx2, @function
icp_isalc_gcm_dec_192_finalize_avx2:
	jmp icp_isalc_gcm_dec_192_finalize_sse

.globl icp_isalc_gcm_enc_192_avx2
.type icp_isalc_gcm_enc_192_avx2, @function
icp_isalc_gcm_enc_192_avx2:
	jmp icp_isalc_gcm_enc_192_sse

.globl icp_isalc_gcm_dec_192_avx2
.type icp_isalc_gcm_dec_192_avx2, @function
icp_isalc_gcm_dec_192_avx2:
	jmp icp_isalc_gcm_dec_192_sse

#endif /* defined(__x86_64__) && defined(HAVE_AVX2) && defined(HAVE_AES) && defined(HAVE_PCLMULQDQ) */