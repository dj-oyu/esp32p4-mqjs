#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Float CELT inner-product replacement boundary.
 *
 * Contract:
 * - a and b each address at least n contiguous floats
 * - a and b may alias
 * - unaligned inputs are accepted
 * - accumulation order is part of the implementation and may introduce
 *   normal floating-point rounding differences
 */
float opus_p4_inner_prod_f32(const float *a, const float *b, int n);
float opus_p4_inner_prod_f32_c(const float *a, const float *b, int n);

/*
 * Fill one collapsed short-MDCT block with deterministic signed noise.
 *
 * Contract:
 * - x addresses n writable floats separated by stride elements
 * - n > 0 and stride > 0
 * - seed progression and sign selection are bit-exact CELT behavior
 * - returns the seed after n LCG steps
 */
uint32_t opus_p4_anti_collapse_noise_f32(float *x, int n, int stride, float r,
                                         uint32_t seed);
uint32_t opus_p4_anti_collapse_noise_f32_c(float *x, int n, int stride, float r,
                                           uint32_t seed);
uint32_t opus_p4_anti_collapse_noise_f32_p4(float *x, int n, int stride, float r,
                                            uint32_t seed);

/*
 * CELT fixed-point comb filter (constant gains), bit-exact with
 * comb_filter_const_c (the non-ARM variant):
 *
 *   for (i=0;i<N;i++)
 *     y[i] = SATURATE(x[i]
 *                     + MULT16_32_Q15(g10, x[i-T])
 *                     + MULT16_32_Q15(g11, x[i-T-1]+x[i-T+1])
 *                     + MULT16_32_Q15(g12, x[i-T+2]+x[i-T-2]), SIG_SAT);
 *
 * Contract:
 * - x points into a buffer where x[-T-2 .. N-1-T+2] are all readable
 *   (T >= 2; the comb filter guarantees the history exists).
 * - y and x do not overlap; y[0..N-1] are writable.
 * - inner adds are 32-bit two's-complement wraparound; MULT16_32_Q15 and the
 *   SIG_SAT=2^29-1 clamp are bit-exact CELT behavior.
 */
void opus_p4_comb_filter_const(int32_t *y, const int32_t *x, int T, int N,
                               int16_t g10, int16_t g11, int16_t g12);
void opus_p4_comb_filter_const_c(int32_t *y, const int32_t *x, int T, int N,
                                 int16_t g10, int16_t g11, int16_t g12);
void opus_p4_comb_filter_const_p4(int32_t *y, const int32_t *x, int T, int N,
                                  int16_t g10, int16_t g11, int16_t g12);

/*
 * CELT denormalise_bands common-path kernel (FIXED_POINT, shift>=0).
 *
 *   f[j] = SHR32(MULT16_32_Q15(x[j], g), shift)   for j in [0, N)
 *
 * Contract:
 * - x points at N contiguous celt_norm (int16); f at N writable celt_sig
 *   (int32). x and f do not overlap.
 * - g is the band-constant int32 gain; shift >= 0 (the common path; the
 *   shift<0 extreme-gain branch stays in C).
 * - The _c reference is bit-exact CELT behavior (same MULT16_32_Q15 and
 *   arithmetic SHR32). The _p4 PIE body is relaxed within opus_compare
 *   tolerance (signed low-half split, like the comb kernel).
 */
void opus_p4_denorm_band_c(int32_t *f, const int16_t *x, int N, int32_t g,
                           int shift);
void opus_p4_denorm_band_p4(int32_t *f, const int16_t *x, int N, int32_t g,
                            int shift);

/* On-device PIE instruction-semantics probe (logs results; ESP32-P4 only,
 * no-op elsewhere). Used to confirm vmul.s32.s16xs16 / cmul.s16 behavior
 * before building the PIE butterfly/comb kernels. */
void opus_p4_pie_probe(void);

const char *opus_p4_kernel_impl(void);
uint32_t opus_p4_kernel_verify_failures(void);

void opus_p4_profile_reset(void);
void opus_p4_profile_dump(int top_n);

#ifdef __cplusplus
}
#endif
