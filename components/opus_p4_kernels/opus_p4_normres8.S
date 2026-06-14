/*
 * ESP32-P4 PIE normalise-residual inner kernel: 8 outputs per call.
 *
 * void opus_p4_normres8(int16_t *X, const int32_t *iy, int16_t g, int k);
 *
 * Computes the CELT normalise_residual hot loop (FIXED_POINT), bit-exact:
 *   X[i] = EXTRACT16(PSHR32(MULT16_16(g, iy[i]), k+1))
 *        = (int16)(( g*(int16)iy[i] + (1<<k) ) >> (k+1))   for i in [0,8)
 *
 * Unlike comb/denorm this is fully bit-exact (no relaxed split): a 16x16 raw
 * product (vmul.s32.s16xs16, SAR=0) gives the exact int32, then an explicit
 * round bias (1<<k) + arithmetic shift (vsr.s32, SAR=k+1) reproduce PSHR32, and
 * vunzip.16 takes the low 16 bits of each lane = the truncating EXTRACT16 cast.
 *
 * iy is int32 (VQ pulses, |iy[i]| < 2^15 so the low s16 half is the value),
 * loaded unaligned via two src.q windows and narrowed to s16 with vunzip.16.
 * Output X is int16 (16 bytes); X must be 16-aligned (the C wrapper routes
 * unaligned X through an aligned temp + memcpy, like comb/denorm).
 *
 * ABI: a0=X, a1=iy, a2=g, a3=k. PIE mem/SAR base GPRs must be x8..x15
 * (a0-a5, s0, s1); only those are used here.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  2
    .global opus_p4_normres8
    .type   opus_p4_normres8,@function

opus_p4_normres8:
    addi    sp, sp, -16
    sh      a2, 0(sp)               /* g (s16) */
    li      a4, 1
    sll     a4, a4, a3              /* 1 << k */
    sw      a4, 4(sp)               /* round bias (s32) */

    /* ---- load iy[0..7] (8 int32 = 32 bytes, unaligned) via two src.q ---- */
    andi    a4, a1, 15
    esp.movx.w.sar.bytes a4         /* SAR_BYTES = iy & 15 */
    andi    a5, a1, -16             /* abase = iy & ~15 */
    esp.vld.128.ip      q1, a5, 16  /* block 0 */
    esp.vld.128.ip      q2, a5, 16  /* block 1 */
    esp.vld.128.ip      q3, a5, 16  /* block 2 */
    esp.src.q           q4, q1, q2  /* iy[0..3] (4 s32) */
    esp.src.q           q5, q2, q3  /* iy[4..7] (4 s32) */
    esp.vunzip.16       q4, q5      /* q4 = low s16 of all 8 = (int16)iy[i] */

    /* ---- broadcasts: g (s16), bias (s32) ---- */
    mv      a4, sp
    esp.vldbc.16.ip     q6, a4, 0   /* q6 = g (sp+0) */
    addi    a4, sp, 4
    esp.vldbc.32.ip     q7, a4, 0   /* q7 = bias (sp+4) */

    /* ---- p = g * iy (raw 16x16->32), SAR=0 ---- */
    li      a4, 0
    esp.movx.w.sar      a4
    esp.vmul.s32.s16xs16 q0, q1, q6, q4   /* q0=lanes0-3, q1=lanes4-7 */

    /* ---- p += bias ; p >>= (k+1) (arithmetic) ---- */
    esp.vadd.s32        q0, q0, q7
    esp.vadd.s32        q1, q1, q7
    addi    a4, a3, 1
    esp.movx.w.sar      a4
    esp.vsr.s32         q0, q0
    esp.vsr.s32         q1, q1

    /* ---- narrow s32->s16 (truncate low halves) = EXTRACT16 ---- */
    esp.vunzip.16       q0, q1      /* q0 = X[0..7] as 8 s16 */
    esp.vst.128.ip      q0, a0, 16

    addi    sp, sp, 16
    ret

    .size opus_p4_normres8, .-opus_p4_normres8

#endif
