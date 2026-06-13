#pragma once

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

const char *opus_p4_kernel_impl(void);

void opus_p4_profile_reset(void);
void opus_p4_profile_dump(int top_n);

#ifdef __cplusplus
}
#endif
