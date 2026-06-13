/*
 * ESP32-P4 anti-collapse signed-noise kernel.
 *
 * uint32_t opus_p4_anti_collapse_noise_f32_p4(
 *     float *x, int n, int stride, float r, uint32_t seed);
 *
 * CELT collapsed-block noise fill, bit-exact with the portable C reference:
 *
 *     for (i = 0; i < n; i++) {
 *         seed = 1664525u * seed + 1013904223u;   // celt_lcg_rand
 *         x[i * stride] = (seed & 0x8000u) ? r : -r;
 *     }
 *     return seed;
 *
 * RISC-V ABI (RV32):
 *   a0 = x        (float *, 4-byte aligned)        -> returns final seed
 *   a1 = n        (int, element count)
 *   a2 = stride   (int, element stride; byte stride = stride*4)
 *   fa0 = r       (float amplitude)
 *   a3 = seed     (uint32 LCG state)
 *
 * Notes for future kernels in this file:
 *   - Scalar integer/FP only. No PIE q registers, no XACC/QACC, no hardware
 *     loop are used here. The LCG carries a serial state dependency and the
 *     store is strided/scattered, so vectorization offers nothing; the win
 *     over C is a branchless sign select (the C ternary branches on a
 *     pseudo-random bit -> ~50% mispredict).
 *   - Sign select is done in the integer domain: nb = bits(-r) = bits(r) ^
 *     0x80000000; the result word is nb ^ (sign bit set iff seed&0x8000).
 *     This reproduces both r and -r bit-for-bit, so the verify harness
 *     (memcmp vs C) passes exactly.
 *   - Only caller-saved registers (a0-a4, t0-t5) are used, so there is no
 *     stack frame.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  2
    .global opus_p4_anti_collapse_noise_f32_p4
    .type   opus_p4_anti_collapse_noise_f32_p4,@function

opus_p4_anti_collapse_noise_f32_p4:
    blez    a1, .Ldone              /* n <= 0: write nothing, return seed */

    fmv.x.w t2, fa0                 /* t2 = bits(r)                       */
    lui     t3, 0x80000            /* t3 = 0x80000000 (sign bit)         */
    xor     t2, t2, t3            /* t2 = nb = bits(-r)                 */
    li      t4, 1664525            /* LCG multiplier                     */
    li      t5, 1013904223         /* LCG increment                      */
    slli    a2, a2, 2              /* a2 = byte stride = stride * 4      */

.Lloop:
    mul     a3, a3, t4             /* seed *= 1664525                    */
    add     a3, a3, t5            /* seed += 1013904223                 */
    slli    t0, a3, 16            /* bit15 -> bit31 (high bits drop)    */
    and     t0, t0, t3           /* isolate sign bit: 0x80000000 or 0  */
    xor     t1, t2, t0           /* word = nb ^ flip                   */
    sw      t1, 0(a0)            /* x[i*stride] = (seed&0x8000)?r:-r   */
    add     a0, a0, a2           /* advance pointer by byte stride     */
    addi    a1, a1, -1
    bnez    a1, .Lloop

.Ldone:
    mv      a0, a3                 /* return final seed                  */
    ret

    .size opus_p4_anti_collapse_noise_f32_p4, .-opus_p4_anti_collapse_noise_f32_p4

#endif
