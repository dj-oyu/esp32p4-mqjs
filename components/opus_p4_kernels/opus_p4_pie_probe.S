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

    /* --- src.q corrected order: in32[0..7]={10,11,...,17} --- */
    esp.vld.128.ip      q0, a1, 16      /* q0 = {10,11,12,13} */
    esp.vld.128.ip      q1, a1, 16      /* q1 = {14,15,16,17} */
    li      a4, 4
    esp.movx.w.sar.bytes a4
    esp.src.q           q2, q0, q1      /* {q1:q0}>>4B -> expect {11,12,13,14} */
    esp.vst.128.ip      q2, a2, 16      /* out[20..23] */

    /* --- auto-unaligned load via ld.128.usar + src.q.ld.ip at &in32[1] --- */
    addi    a4, a1, -28                 /* a4 = &in32[1] (a1 advanced 32; back to in32, +4) */
    esp.ld.128.usar.ip  q3, a4, 16      /* q3=aligned blk, SAR_BYTES=in32[1]&15=4 */
    esp.src.q.ld.ip     q4, a4, 16, q3, q3 /* q4 = unaligned [in32[1..4]] = {11,12,13,14} */
    esp.vst.128.ip      q4, a2, 16      /* out[24..27] */

    /* --- vunzip.32 / vzip.32 lane order (complex r/i de/interleave) ---
     * in32[0..7]={10..17} read as 4 complex (10,11)(12,13)(14,15)(16,17).
     * vunzip.32 should split even/odd 32-bit lanes:
     *   q0 -> {10,12,14,16} (r's), q1 -> {11,13,15,17} (i's).
     * vzip.32 should round-trip back to {10,11,12,13}/{14,15,16,17}. */
    addi    a4, a1, -32                 /* back to &in32[0] (a1 advanced 32) */
    esp.vld.128.ip      q0, a4, 16      /* {10,11,12,13} */
    esp.vld.128.ip      q1, a4, 16      /* {14,15,16,17} */
    esp.vunzip.32       q0, q1
    esp.vst.128.ip      q0, a2, 16      /* out[28..31] expect 10,12,14,16 */
    esp.vst.128.ip      q1, a2, 16      /* out[32..35] expect 11,13,15,17 */
    esp.vzip.32         q0, q1
    esp.vst.128.ip      q0, a2, 16      /* out[36..39] expect 10,11,12,13 */

    ret

    .size opus_p4_pie_probe_run, .-opus_p4_pie_probe_run

#endif
