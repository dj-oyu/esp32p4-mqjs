/*
 * ESP32-P4 PIE denormalise-bands inner kernel: 8 outputs per call.
 *
 * void opus_p4_denorm8(int32_t *f, const int16_t *x, int32_t g, int shift);
 *
 * Computes the CELT denormalise_bands common path (FIXED_POINT, shift>=0):
 *   f[k] = SHR32(MULT16_32_Q15(x[k], g), shift)   for k in [0,8)
 * with the same signed 16x32 split the comb kernel uses:
 *   M(g,v) = (g_hi * v) * 2 + ((g_lo * v) >> 15),  g_hi=(s16)(g>>16), g_lo=(s16)g
 * then f = m >> shift. Relaxed (g_lo treated signed instead of the codec's
 * unsigned low half) -> ~-78 dB error, within opus_compare tolerance.
 *
 * Simpler than comb8: x is contiguous int16, so one unaligned src.q load yields
 * all 8 s16 lanes directly (no per-tap windows, no vunzip). The 8 outputs are
 * int32 (32 bytes = 2 q regs); f must be 16-aligned (the C wrapper routes
 * unaligned f through an aligned temp + memcpy, like comb).
 *
 * NOTE: esp.vmul.s32.s16xs16 right-shifts its product by SAR; SAR=0 for the raw
 * g_hi product, SAR=15 for the >>15 low product. SAR_BYTES (src.q) and SAR
 * (vmul/vsr) are independent registers.
 *
 * ABI: a0=f, a1=x, a2=g, a3=shift. PIE mem/SAR base GPRs must be x8..x15
 * (a0-a5, s0, s1); only those are used here.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  2
    .global opus_p4_denorm8
    .type   opus_p4_denorm8,@function

opus_p4_denorm8:
    addi    sp, sp, -16
    srai    a4, a2, 16              /* g_hi = (s16)(g >> 16) */
    sh      a4, 0(sp)
    sh      a2, 2(sp)               /* g_lo = (s16)(g & 0xffff) */

    /* ---- load x[0..7] (8 int16, unaligned) via src.q funnel ---- */
    andi    a4, a1, 15
    esp.movx.w.sar.bytes a4         /* SAR_BYTES = x & 15 */
    andi    a5, a1, -16             /* abase = x & ~15 */
    esp.vld.128.ip      q1, a5, 16  /* block 0 */
    esp.vld.128.ip      q2, a5, 16  /* block 1 */
    esp.src.q           q0, q1, q2  /* q0 = x[0..7] as 8 s16 */

    /* ---- broadcast g_hi, g_lo to all 8 lanes (vldbc.16.ip imm must step 4) ---- */
    mv      a4, sp
    esp.vldbc.16.ip     q3, a4, 0   /* q3 = g_hi (sp+0) */
    addi    a4, sp, 2
    esp.vldbc.16.ip     q4, a4, 0   /* q4 = g_lo (sp+2) */

    /* ---- phi = g_hi * x (raw), SAR=0 ; doubled ---- */
    li      a4, 0
    esp.movx.w.sar      a4
    esp.vmul.s32.s16xs16 q5, q6, q3, q0   /* q5=lanes0-3, q6=lanes4-7 */
    esp.vadd.s32        q5, q5, q5         /* *2 */
    esp.vadd.s32        q6, q6, q6

    /* ---- plo = (g_lo * x) >> 15, SAR=15 ; m = phi*2 + plo ---- */
    li      a4, 15
    esp.movx.w.sar      a4
    esp.vmul.s32.s16xs16 q1, q2, q4, q0
    esp.vadd.s32        q5, q5, q1
    esp.vadd.s32        q6, q6, q2

    /* ---- f = m >> shift (arithmetic), SAR=shift ---- */
    esp.movx.w.sar      a3
    esp.vsr.s32         q5, q5
    esp.vsr.s32         q6, q6

    esp.vst.128.ip      q5, a0, 16  /* f[0..3] */
    esp.vst.128.ip      q6, a0, 16  /* f[4..7] */

    addi    sp, sp, 16
    ret

    .size opus_p4_denorm8, .-opus_p4_denorm8

#endif
