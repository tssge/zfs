/*
 * SPDX-License-Identifier: CDDL-1.0
 * 
 * Placeholder AVX2 implementation for AES-GCM 128-bit keys
 * This is a minimal stub that falls back to SSE implementation
 * TODO: Implement optimized AVX2 routines
 */

#if defined(__x86_64__) && defined(HAVE_AVX2) && defined(HAVE_AES) && \
    defined(HAVE_PCLMULQDQ)

/*
 * For now, provide simple jump stubs to SSE implementations.
 * This allows the build system to resolve symbols while we implement 
 * the full AVX2 optimization later.
 */

.text

.globl icp_isalc_gcm_precomp_128_avx2
.type icp_isalc_gcm_precomp_128_avx2, @function
icp_isalc_gcm_precomp_128_avx2:
	jmp icp_isalc_gcm_precomp_128_sse

.globl icp_isalc_gcm_init_128_avx2
.type icp_isalc_gcm_init_128_avx2, @function
icp_isalc_gcm_init_128_avx2:
	jmp icp_isalc_gcm_init_128_sse

.globl icp_isalc_gcm_enc_128_update_avx2
.type icp_isalc_gcm_enc_128_update_avx2, @function
icp_isalc_gcm_enc_128_update_avx2:
	jmp icp_isalc_gcm_enc_128_update_sse

.globl icp_isalc_gcm_dec_128_update_avx2
.type icp_isalc_gcm_dec_128_update_avx2, @function
icp_isalc_gcm_dec_128_update_avx2:
	jmp icp_isalc_gcm_dec_128_update_sse

.globl icp_isalc_gcm_enc_128_finalize_avx2
.type icp_isalc_gcm_enc_128_finalize_avx2, @function
icp_isalc_gcm_enc_128_finalize_avx2:
	jmp icp_isalc_gcm_enc_128_finalize_sse

.globl icp_isalc_gcm_dec_128_finalize_avx2
.type icp_isalc_gcm_dec_128_finalize_avx2, @function
icp_isalc_gcm_dec_128_finalize_avx2:
	jmp icp_isalc_gcm_dec_128_finalize_sse

.globl icp_isalc_gcm_enc_128_avx2
.type icp_isalc_gcm_enc_128_avx2, @function
icp_isalc_gcm_enc_128_avx2:
	jmp icp_isalc_gcm_enc_128_sse

.globl icp_isalc_gcm_dec_128_avx2
.type icp_isalc_gcm_dec_128_avx2, @function
icp_isalc_gcm_dec_128_avx2:
	jmp icp_isalc_gcm_dec_128_sse

#endif /* defined(__x86_64__) && defined(HAVE_AVX2) && defined(HAVE_AES) && defined(HAVE_PCLMULQDQ) */