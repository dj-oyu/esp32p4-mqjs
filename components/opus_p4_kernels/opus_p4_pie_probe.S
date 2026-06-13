/*
 * On-device PIE semantics probe.
 *
 * void opus_p4_pie_probe_run(const int16_t *in, int32_t *out);
 *   in : 16-byte aligned, >= 32 int16 (four 128-bit vectors)
 *   out: 16-byte aligned, >= 16 int32 (four 128-bit vectors)
 *
 * in[0..7]   : A for the widening multiply (8 x s16)
 * in[8..15]  : B for the widening multiply (8 x s16)
 * in[16..23] : data for cmul (4 complex sc16: r,i,r,i,r,i,r,i)
 * in[24..31] : twiddle for cmul (4 complex sc16)
 *
 * out[0..3]  : vmul.s32.s16xs16 dst0 (q2)
 * out[4..7]  : vmul.s32.s16xs16 dst1 (q3)
 * out[8..11] : cmul.s16 result after the sel 2 + sel 3 two-pass (q6), as packed sc16
 *
 * Decodes: (a) which products land in which dst lanes for vmul.s32.s16xs16,
 * (b) whether cmul.s16 with sel 2 then 3 produces a full complex product and
 * the real/imag lane layout.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  2
    .global opus_p4_pie_probe_run
    .type   opus_p4_pie_probe_run,@function

opus_p4_pie_probe_run:
    /* a0 = in, a1 = out */
    esp.vld.128.ip      q0, a0, 16      /* q0 = in[0..7]   (8 x s16) */
    esp.vld.128.ip      q1, a0, 16      /* q1 = in[8..15]  (8 x s16) */
    esp.vmul.s32.s16xs16 q2, q3, q0, q1 /* q2,q3 = 8 x (s16*s16 -> s32) */
    esp.vst.128.ip      q2, a1, 16      /* out[0..3] */
    esp.vst.128.ip      q3, a1, 16      /* out[4..7] */

    esp.vld.128.ip      q4, a0, 16      /* q4 = in[16..23] cmul data    */
    esp.vld.128.ip      q5, a0, 16      /* q5 = in[24..31] cmul twiddle */
    esp.cmul.s16        q6, q5, q4, 2   /* esp-dsp two-pass complex mul  */
    esp.cmul.s16        q6, q5, q4, 3
    esp.vst.128.ip      q6, a1, 16      /* out[8..11] (packed sc16)      */
    ret

    .size opus_p4_pie_probe_run, .-opus_p4_pie_probe_run

#endif
