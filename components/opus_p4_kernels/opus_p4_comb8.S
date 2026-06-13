/*
 * ESP32-P4 PIE comb-filter inner kernel: 8 outputs per call.
 *
 * void opus_p4_comb8(int32_t *y, const int32_t *x, int T,
 *                    int16_t g10, int16_t g11, int16_t g12);
 *
 * For k = 0..7:
 *   s = x[k-T]; a = x[k-T+1]+x[k-T-1]; b = x[k-T+2]+x[k-T-2];
 *   t = x[k] + M(g10,s) + M(g11,a) + M(g12,b);  y[k] = clamp(t, +/-SIG_SAT)
 * with relaxed M(g,v) = (g*hi)*2 + (g*lo)>>15, signed split hi=(s16)(v>>16),
 * lo=(s16)v (vunzip.16).
 *
 * NOTE: esp.vmul.s32.s16xs16 right-shifts its product by SAR. So the hi
 * product is taken with SAR=0 (raw g*hi, then *2 via vadd) and the lo product
 * with SAR=15 (= (g*lo)>>15 directly). SAR is set per term right before each
 * multiply.
 *
 * ABI: a0=y, a1=x (=&x[i]), a2=T, a3=g10, a4=g11, a5=g12.
 * PIE memory / SAR ops require base GPRs in x8..x15 (a0-a5, s0, s1).
 *
 * KNOWN BUG (WIP): esp.vld.128 is 16-byte-aligned only -- it drops the low
 * address bits. The shifted comb taps (x[i-T+/-1], x[i-T+/-2] at s0+/-4, s0+/-8)
 * are not 16-aligned, so those loads read wrong data. Verified correct only
 * when every tap address is 16-aligned. Fix requires unaligned loads via
 * esp.ld.128.usar + esp.src.q funnel-shift (the esp-dsp FIR pattern). The
 * arithmetic (16x32 via vmul.s32.s16xs16, SAR-controlled shift, saturate) and
 * the ~3x speedup are confirmed; only the windowed loads need the usar path.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  2
    .global opus_p4_comb8
    .type   opus_p4_comb8,@function

    /* term: q2=lo, q3=hi (after vunzip), q4=g broadcast.
     * accumulates into t (q0,q1). Clobbers a4,q2,q3,q5,q6,q7. */
    .macro COMB_TERM
    li      a4, 0
    esp.movx.w.sar      a4
    esp.vmul.s32.s16xs16 q5, q6, q4, q3   /* phi = g*hi (raw, SAR=0) */
    esp.vadd.s32        q3, q5, q5  /* phi*2 -> q3,q7 */
    esp.vadd.s32        q7, q6, q6
    esp.vmul.s32.s16xs16 q5, q6, q4, q2   /* plo = g*lo (raw, SAR=0) */
    li      a4, 15
    esp.movx.w.sar      a4
    esp.vsr.s32         q5, q5      /* plo>>15 (arithmetic) */
    esp.vsr.s32         q6, q6
    esp.vadd.s32        q3, q3, q5  /* m = phi*2 + plo>>15 */
    esp.vadd.s32        q7, q7, q6
    esp.vadd.s32        q0, q0, q3  /* t += m */
    esp.vadd.s32        q1, q1, q7
    .endm

opus_p4_comb8:
    addi    sp, sp, -32
    sw      s0, 0(sp)
    sw      s1, 4(sp)
    sh      a3, 8(sp)               /* g10 */
    sh      a4, 10(sp)              /* g11 */
    sh      a5, 12(sp)              /* g12 */
    li      a6, 536870911           /* SIG_SAT = 2^29-1 */
    sw      a6, 16(sp)
    li      a6, -536870911
    sw      a6, 20(sp)

    slli    a6, a2, 2               /* a6 = 4*T (bytes) */
    sub     s0, a1, a6              /* s0 = &x[i-T] */
    addi    s1, sp, 8              /* s1 -> g10/g11/g12 (vldbc.16 stream) */

    esp.vld.128.ip      q0, a1, 16  /* t = x[i..i+3]  */
    esp.vld.128.ip      q1, a1, 16  /* t = x[i+4..i+7]*/

    /* ---- term g10:  s = x[i-T..]  ---- */
    mv      a3, s0
    esp.vld.128.ip      q2, a3, 16
    esp.vld.128.ip      q3, a3, 16
    esp.vunzip.16       q2, q3      /* q2=lo s16, q3=hi s16 */
    esp.vldbc.16.ip     q4, s1, 2   /* g10 */
    COMB_TERM

    /* ---- term g11:  a = x[i-T+1..] + x[i-T-1..]  ---- */
    addi    a3, s0, 4
    esp.vld.128.ip      q2, a3, 16
    esp.vld.128.ip      q3, a3, 16
    addi    a3, s0, -4
    esp.vld.128.ip      q5, a3, 16
    esp.vld.128.ip      q6, a3, 16
    esp.vadd.s32        q2, q2, q5
    esp.vadd.s32        q3, q3, q6
    esp.vunzip.16       q2, q3
    esp.vldbc.16.ip     q4, s1, 2   /* g11 */
    COMB_TERM

    /* ---- term g12:  b = x[i-T+2..] + x[i-T-2..]  ---- */
    addi    a3, s0, 8
    esp.vld.128.ip      q2, a3, 16
    esp.vld.128.ip      q3, a3, 16
    addi    a3, s0, -8
    esp.vld.128.ip      q5, a3, 16
    esp.vld.128.ip      q6, a3, 16
    esp.vadd.s32        q2, q2, q5
    esp.vadd.s32        q3, q3, q6
    esp.vunzip.16       q2, q3
    esp.vldbc.16.ip     q4, s1, 2   /* g12 */
    COMB_TERM

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
    lw      s1, 4(sp)
    addi    sp, sp, 32
    ret

    .size opus_p4_comb8, .-opus_p4_comb8

#endif
