/*
 * On-device PIE semantics probe.
 *
 * void opus_p4_pie_probe_run(const int16_t *in, const int32_t *in32,
 *                            int32_t *out);
 *
 * in  (16B aligned, >=32 s16): [0..7]=A, [8..15]=B (widening mul);
 *                              [16..23]=cmul data, [24..31]=cmul twiddle
 * in32(16B aligned, >=9 s32):  [0..3]=shift test, [4..7]=clamp test, [8]=SIG_SAT
 * out (16B aligned, >=32 s32): see stores below
 *
 * Confirms the instructions the comb/butterfly PIE kernels need:
 *   vmul.s32.s16xs16, vunzip.16 (s32<->s16 marshaling), movx.w.sar + vsr.s32 /
 *   vsl.32 (shift), vmin.s32 + vldbc.32 (saturate), cmul.s16.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  2
    .global opus_p4_pie_probe_run
    .type   opus_p4_pie_probe_run,@function

opus_p4_pie_probe_run:
    /* a0 = in, a1 = in32, a2 = out */

    /* --- vmul.s32.s16xs16 --- */
    esp.vld.128.ip      q0, a0, 16      /* in[0..7]  */
    esp.vld.128.ip      q1, a0, 16      /* in[8..15] */
    esp.vmul.s32.s16xs16 q2, q3, q0, q1
    esp.vst.128.ip      q2, a2, 16      /* out[0..3]  */
    esp.vst.128.ip      q3, a2, 16      /* out[4..7]  */

    /* --- cmul.s16 (esp-dsp sel 2 + 3 two-pass) --- */
    esp.vld.128.ip      q4, a0, 16      /* in[16..23] data    */
    esp.vld.128.ip      q5, a0, 16      /* in[24..31] twiddle */
    esp.cmul.s16        q6, q5, q4, 2
    esp.cmul.s16        q6, q5, q4, 3
    esp.vst.128.ip      q6, a2, 16      /* out[8..11] */

    /* --- vunzip.16: reload A={1..8}, B={10..80} from in[0..15] --- */
    addi    a3, a0, -64                 /* a0 advanced 4*16; back to in[0] */
    esp.vld.128.ip      q0, a3, 16
    esp.vld.128.ip      q1, a3, 16
    esp.vunzip.16       q0, q1
    esp.vst.128.ip      q0, a2, 16      /* out[12..15] */
    esp.vst.128.ip      q1, a2, 16      /* out[16..19] */

    /* --- comb inner pipeline trace on const int32 in32[0..7]=0x100000 --- */
    esp.vld.128.ip      q0, a1, 16      /* q0 = in32[0..3] */
    esp.vld.128.ip      q1, a1, 16      /* q1 = in32[4..7] */
    esp.vunzip.16       q0, q1          /* q0=lo, q1=hi */
    esp.vst.128.ip      q0, a2, 16      /* out[20..23] = lo (expect 0) */
    esp.vst.128.ip      q1, a2, 16      /* out[24..27] = hi (expect 16) */
    esp.vldbc.16.ip     q4, a1, 0       /* q4 = bcast in32[8] low16 = g */
    esp.vmul.s32.s16xs16 q5, q6, q4, q1 /* phi = g*hi */
    esp.vst.128.ip      q5, a2, 16      /* out[28..31] = phi (expect g*16) */
    li      a4, 1
    esp.movx.w.sar      a4
    esp.vsl.32          q2, q5          /* phi<<1 */
    esp.vst.128.ip      q2, a2, 16      /* out[32..35] = phi<<1 (expect g*32) */
    esp.vadd.s32        q5, q5, q2      /* in-place: phi + phi<<1 = 144000+288000 */
    esp.vst.128.ip      q5, a2, 16      /* out[36..39] (expect 432000) */

    ret

    .size opus_p4_pie_probe_run, .-opus_p4_pie_probe_run

#endif
