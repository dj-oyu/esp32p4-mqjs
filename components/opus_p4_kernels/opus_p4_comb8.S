/*
 * ESP32-P4 PIE comb-filter inner kernel: 8 outputs per call.
 *
 * void opus_p4_comb8(int32_t *y, const int32_t *x, int T,
 *                    int16_t g10, int16_t g11, int16_t g12);
 *
 * Relaxed comb (opus_compare tolerance, not bit-exact): the symmetric taps are
 * split into 5 independent MULT16_32_Q15 terms,
 *   y[k] = x[k] + M(g10,x[k-T]) + M(g11,x[k-T-1]) + M(g11,x[k-T+1])
 *               + M(g12,x[k-T-2]) + M(g12,x[k-T+2])
 * clamped to +/-SIG_SAT. M(g,v) = (g*hi)*2 + (g*lo)>>15 with the signed split
 * hi=(s16)(v>>16), lo=(s16)v (vunzip.16).
 *
 * Each tap window is unaligned (x[k-T+d] is not 16-aligned). PIE vld.128 is
 * aligned-only, so each tap loads 3 aligned blocks from its own aligned base
 * and funnel-shifts (esp.src.q with SAR_BYTES = P&15) to recover the window.
 * SAR_BYTES (src.q) and SAR (vmul/vsr) are independent registers.
 *
 * Verified on device: maxdiff vs scalar ~5.4e4 (~-64 dB, the relaxed split
 * error), ~2x faster than scalar. Requires the 8-output blocks (x+k) to be
 * 16-aligned, which holds when the caller's x is 16-aligned and k is a
 * multiple of 8.
 *
 * NOTE: esp.vmul.s32.s16xs16 right-shifts its product by SAR; use SAR=0 for the
 * raw g*hi product, SAR=15 for the >>15.
 *
 * ABI: a0=y, a1=x (=&x[i]), a2=T, a3=g10, a4=g11, a5=g12.
 * PIE memory / SAR ops require base GPRs in x8..x15 (a0-a5, s0, s1).
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  2
    .global opus_p4_comb8
    .type   opus_p4_comb8,@function

    /* One tap: d_bytes = byte offset of x[k-T+d] from s0; g_off = stack offset
     * of the s16 gain. Accumulates M(g, tap) into t (q0,q1). */
    .macro TAP d_bytes, g_off
    addi    a3, s0, \d_bytes        /* P = &x[k-T+d] */
    andi    a4, a3, 15
    esp.movx.w.sar.bytes a4         /* SAR_BYTES = P & 15 */
    andi    a3, a3, -16             /* abase = P & ~15 */
    esp.vld.128.ip      q2, a3, 16  /* Ba */
    esp.vld.128.ip      q3, a3, 16  /* Bb */
    esp.vld.128.ip      q4, a3, 16  /* Bc */
    esp.src.q           q5, q2, q3  /* win lo = x[P..P+3] */
    esp.src.q           q6, q3, q4  /* win hi = x[P+4..P+7] */
    esp.vunzip.16       q5, q6      /* q5=lo s16, q6=hi s16 */
    addi    a4, sp, \g_off
    esp.vldbc.16.ip     q7, a4, 0   /* q7 = g broadcast */
    li      a4, 0
    esp.movx.w.sar      a4
    esp.vmul.s32.s16xs16 q2, q3, q7, q6   /* phi = g*hi (raw) */
    esp.vadd.s32        q2, q2, q2  /* phi*2 */
    esp.vadd.s32        q3, q3, q3
    esp.vmul.s32.s16xs16 q5, q6, q7, q5   /* plo = g*lo (raw) */
    li      a4, 15
    esp.movx.w.sar      a4
    esp.vsr.s32         q5, q5      /* plo>>15 */
    esp.vsr.s32         q6, q6
    esp.vadd.s32        q2, q2, q5  /* m = phi*2 + plo>>15 */
    esp.vadd.s32        q3, q3, q6
    esp.vadd.s32        q0, q0, q2  /* t += m */
    esp.vadd.s32        q1, q1, q3
    .endm

opus_p4_comb8:
    addi    sp, sp, -32
    sw      s0, 0(sp)
    sh      a3, 8(sp)               /* g10 */
    sh      a4, 10(sp)              /* g11 */
    sh      a5, 12(sp)              /* g12 */
    li      a6, 536870911           /* SIG_SAT = 2^29-1 */
    sw      a6, 16(sp)
    li      a6, -536870911
    sw      a6, 20(sp)

    slli    a6, a2, 2               /* a6 = 4*T (bytes) */
    sub     s0, a1, a6              /* s0 = &x[k-T] */

    /* t = x[k..k+7], possibly unaligned -> src.q funnel like a tap */
    mv      a3, a1
    andi    a4, a3, 15
    esp.movx.w.sar.bytes a4
    andi    a3, a3, -16
    esp.vld.128.ip      q2, a3, 16
    esp.vld.128.ip      q3, a3, 16
    esp.vld.128.ip      q4, a3, 16
    esp.src.q           q0, q2, q3  /* t = x[k..k+3]  */
    esp.src.q           q1, q3, q4  /* t = x[k+4..k+7]*/

    TAP     0, 8                     /* d= 0, g10 */
    TAP     -4, 10                   /* d=-1, g11 */
    TAP     4, 10                    /* d=+1, g11 */
    TAP     -8, 12                   /* d=-2, g12 */
    TAP     8, 12                    /* d=+2, g12 */

    /* ---- saturate to +/- SIG_SAT and store ---- */
    addi    a3, sp, 16
    esp.vldbc.32.ip     q4, a3, 4   /* q4 = +SIG_SAT, a3 -> -SIG_SAT */
    esp.vmin.s32        q0, q0, q4
    esp.vmin.s32        q1, q1, q4
    esp.vldbc.32.ip     q5, a3, 0   /* q5 = -SIG_SAT */
    esp.vmax.s32        q0, q0, q5
    esp.vmax.s32        q1, q1, q5
    esp.vst.128.ip      q0, a0, 16
    esp.vst.128.ip      q1, a0, 16

    lw      s0, 0(sp)
    addi    sp, sp, 32
    ret

    .size opus_p4_comb8, .-opus_p4_comb8

#endif
