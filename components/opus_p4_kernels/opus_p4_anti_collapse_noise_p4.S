/*
 * ESP32-P4 anti-collapse signed-noise kernel ABI scaffold.
 *
 * uint32_t opus_p4_anti_collapse_noise_f32_p4(
 *     float *x, int n, int stride, float r, uint32_t seed);
 *
 * RISC-V ABI:
 *   a0=x, a1=n, a2=stride, fa0=r, a3=seed, return seed in a0.
 *
 * This intentionally contains no optimized assembly yet. It validates the
 * build, symbol, calling convention, dispatch, and C-verification paths before
 * the body is replaced with scalar or PIE assembly. Keep the public ABI and C
 * reference unchanged when replacing this tail call.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  2
    .global opus_p4_anti_collapse_noise_f32_p4
    .type   opus_p4_anti_collapse_noise_f32_p4,@function

opus_p4_anti_collapse_noise_f32_p4:
    tail opus_p4_anti_collapse_noise_f32_c

    .size opus_p4_anti_collapse_noise_f32_p4, .-opus_p4_anti_collapse_noise_f32_p4

#endif
