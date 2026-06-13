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

const char *opus_p4_kernel_impl(void);
uint32_t opus_p4_kernel_verify_failures(void);

void opus_p4_profile_reset(void);
void opus_p4_profile_dump(int top_n);

#ifdef __cplusplus
}
#endif
