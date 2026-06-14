/*
 * ESP32-P4 PIE kf_bfly3 inner kernel: 4 complex (k values) per call.
 *
 * void opus_p4_bfly3_4(int32_t *fa, int32_t *fb, int32_t *fc,
 *                      const int16_t *tw1, const int16_t *tw2, int16_t epi3);
 *
 * fa/fb/fc each point at 4 interleaved int32 complex {r,i} (32 bytes, aligned);
 * tw1/tw2 at 4 interleaved int16 complex (16 bytes, aligned). Computes one
 * radix-3 butterfly over the 4 lanes, bit-exact with opus_p4_bfly3_c:
 *   s1 = C_MUL(fb,tw1); s2 = C_MUL(fc,tw2)
 *   s3 = s1+s2; s0 = s1-s2
 *   fb = fa - HALF(s3);  s0 *= epi3;  fa += s3
 *   fc = (fb.r + s0.i, fb.i - s0.r);  fb = (fb.r - s0.i, fb.i + s0.r)
 * where S_MUL(t,d)=MULT16_32_Q15(t,d) via the comb signed split
 *   M(t,d) = (t*d_hi)*2 + (t*d_lo)>>15,  d_hi=(s16)(d>>16), d_lo=(s16)d.
 *
 * 4-wide: complex int32 data is one qreg of 4 lanes; vunzip.32 splits r/i,
 * vunzip.16 splits each int32 vector into lo/hi s16 (lanes 0-3 valid). The
 * 8 QRs cannot hold all of fb/fc/s1/s2 split at once, so the data splits and
 * the s1/s2/s3/s0 vectors live in a stack scratch frame; products are computed
 * one at a time. esp.vmul.s32.s16xs16 writes a dst PAIR (lo lanes 0-3 = the
 * valid 4 products, hi = ignored junk), so each product needs a junk qreg.
 *
 * ABI: a0=fa,a1=fb,a2=fc,a3=tw1,a4=tw2,a5=epi3. PIE base GPRs must be x8..x15;
 * s0 (x8) = scratch base, s1 (x9) = transient address / SAR value. a0/a1/a2 are
 * post-incremented by their loads and reset before the stores.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    /* stack scratch slot offsets (from s0) */
    .equ  RLO, 0      /* current data real lo-s16 */
    .equ  RHI, 16
    .equ  ILO, 32
    .equ  IHI, 48
    .equ  TR,  64     /* twiddle real (lanes 0-3) */
    .equ  TI,  80
    .equ  S1R, 96
    .equ  S1I, 112
    .equ  S2R, 128
    .equ  S2I, 144
    .equ  FRAME, 176  /* 160 scratch + saved s0,s1 + pad */

    .text
    .align  2
    .global opus_p4_bfly3_4
    .type   opus_p4_bfly3_4,@function

    /* M(t,d) = (t*d_hi)*2 + (t*d_lo)>>15 into \dst (lanes 0-3).
     * \plo,\jk scratch qregs; \tq=twiddle s16, \dhiq=d_hi s16, \dloq=d_lo s16. */
    .macro PROD dst, plo, jk, tq, dhiq, dloq
    li      s1, 0
    esp.movx.w.sar      s1
    esp.vmul.s32.s16xs16 \dst, \jk, \tq, \dhiq
    esp.vadd.s32        \dst, \dst, \dst
    li      s1, 15
    esp.movx.w.sar      s1
    esp.vmul.s32.s16xs16 \plo, \jk, \tq, \dloq
    esp.vadd.s32        \dst, \dst, \plo
    .endm

    /* Load 4 complex int32 from \ptr (post-inc 32), split r/i then lo/hi s16,
     * store RLO/RHI/ILO/IHI. Clobbers q5,q6,q7. */
    .macro LOADSPLIT ptr
    esp.vld.128.ip      q5, \ptr, 16
    esp.vld.128.ip      q6, \ptr, 16
    esp.vunzip.32       q5, q6          /* q5=r[4], q6=i[4] */
    esp.orq             q7, q5, q5
    esp.vunzip.16       q5, q7          /* q5=r_lo, q7=r_hi */
    addi    s1, s0, RLO
    esp.vst.128.ip      q5, s1, 0
    addi    s1, s0, RHI
    esp.vst.128.ip      q7, s1, 0
    esp.orq             q7, q6, q6
    esp.vunzip.16       q6, q7          /* q6=i_lo, q7=i_hi */
    addi    s1, s0, ILO
    esp.vst.128.ip      q6, s1, 0
    addi    s1, s0, IHI
    esp.vst.128.ip      q7, s1, 0
    .endm

    /* Load 4 complex int16 twiddles from \ptr (post-inc 16), split to TR/TI. */
    .macro TWSPLIT ptr
    esp.vld.128.ip      q5, \ptr, 16
    esp.orq             q7, q5, q5
    esp.vunzip.16       q5, q7          /* q5=tr[4] (even), q7=ti[4] (odd) */
    addi    s1, s0, TR
    esp.vst.128.ip      q5, s1, 0
    addi    s1, s0, TI
    esp.vst.128.ip      q7, s1, 0
    .endm

    /* C_MUL of the data currently in RLO/RHI/ILO/IHI by twiddle in TR/TI,
     * storing s.r -> \sr_off and s.i -> \si_off. q0..q7 free on entry. */
    .macro CMUL sr_off, si_off
    /* A = M(tr, dr) */
    addi    s1, s0, TR
    esp.vld.128.ip      q2, s1, 0
    addi    s1, s0, RHI
    esp.vld.128.ip      q3, s1, 0
    addi    s1, s0, RLO
    esp.vld.128.ip      q4, s1, 0
    PROD    q0, q5, q6, q2, q3, q4      /* q0 = A */
    /* B = M(ti, di) */
    addi    s1, s0, TI
    esp.vld.128.ip      q2, s1, 0
    addi    s1, s0, IHI
    esp.vld.128.ip      q3, s1, 0
    addi    s1, s0, ILO
    esp.vld.128.ip      q4, s1, 0
    PROD    q1, q5, q6, q2, q3, q4      /* q1 = B */
    esp.vsub.s32        q0, q0, q1      /* s.r = A - B */
    addi    s1, s0, \sr_off
    esp.vst.128.ip      q0, s1, 0
    /* C = M(tr, di) */
    addi    s1, s0, TR
    esp.vld.128.ip      q2, s1, 0
    addi    s1, s0, IHI
    esp.vld.128.ip      q3, s1, 0
    addi    s1, s0, ILO
    esp.vld.128.ip      q4, s1, 0
    PROD    q0, q5, q6, q2, q3, q4      /* q0 = C */
    /* D = M(ti, dr) */
    addi    s1, s0, TI
    esp.vld.128.ip      q2, s1, 0
    addi    s1, s0, RHI
    esp.vld.128.ip      q3, s1, 0
    addi    s1, s0, RLO
    esp.vld.128.ip      q4, s1, 0
    PROD    q1, q5, q6, q2, q3, q4      /* q1 = D */
    esp.vadd.s32        q0, q0, q1      /* s.i = C + D */
    addi    s1, s0, \si_off
    esp.vst.128.ip      q0, s1, 0
    .endm

opus_p4_bfly3_4:
    addi    sp, sp, -FRAME
    sw      s0, FRAME-8(sp)
    sw      s1, FRAME-16(sp)
    addi    s0, sp, 0                   /* s0 = scratch base */

    /* ---- s1 = C_MUL(fb, tw1) ---- */
    LOADSPLIT a1                        /* fb -> RLO/RHI/ILO/IHI */
    TWSPLIT a3                          /* tw1 -> TR/TI */
    CMUL    S1R, S1I

    /* ---- s2 = C_MUL(fc, tw2) ---- */
    LOADSPLIT a2                        /* fc -> RLO/.. */
    TWSPLIT a4                          /* tw2 -> TR/TI */
    CMUL    S2R, S2I

    /* ---- s3 = s1+s2 ; s0 = s1-s2 ; stash s3 in S1*, s0 in S2* ---- */
    addi    s1, s0, S1R
    esp.vld.128.ip      q0, s1, 0       /* s1r */
    addi    s1, s0, S1I
    esp.vld.128.ip      q1, s1, 0       /* s1i */
    addi    s1, s0, S2R
    esp.vld.128.ip      q2, s1, 0       /* s2r */
    addi    s1, s0, S2I
    esp.vld.128.ip      q3, s1, 0       /* s2i */
    esp.vadd.s32        q4, q0, q2      /* s3r */
    esp.vadd.s32        q5, q1, q3      /* s3i */
    esp.vsub.s32        q6, q0, q2      /* s0r */
    esp.vsub.s32        q7, q1, q3      /* s0i */
    addi    s1, s0, S1R
    esp.vst.128.ip      q4, s1, 0       /* S1R = s3r */
    addi    s1, s0, S1I
    esp.vst.128.ip      q5, s1, 0       /* S1I = s3i */
    addi    s1, s0, S2R
    esp.vst.128.ip      q6, s1, 0       /* S2R = s0r */
    addi    s1, s0, S2I
    esp.vst.128.ip      q7, s1, 0       /* S2I = s0i */

    /* ---- s0 *= epi3 : s0r=M(epi3,s0r), s0i=M(epi3,s0i) ---- */
    sh      a5, TR(s0)                  /* stash epi3 scalar */
    addi    s1, s0, TR
    esp.vldbc.16.ip     q2, s1, 0       /* q2 = epi3 broadcast */
    /* s0r */
    addi    s1, s0, S2R
    esp.vld.128.ip      q3, s1, 0
    esp.orq             q4, q3, q3
    esp.vunzip.16       q3, q4          /* q3=s0r_lo, q4=s0r_hi */
    PROD    q5, q6, q7, q2, q4, q3      /* q5 = M(epi3, s0r) */
    addi    s1, s0, S2R
    esp.vst.128.ip      q5, s1, 0       /* S2R = s0r' */
    /* s0i */
    addi    s1, s0, S2I
    esp.vld.128.ip      q3, s1, 0
    esp.orq             q4, q3, q3
    esp.vunzip.16       q3, q4
    PROD    q5, q6, q7, q2, q4, q3      /* q5 = M(epi3, s0i) */
    addi    s1, s0, S2I
    esp.vst.128.ip      q5, s1, 0       /* S2I = s0i' */

    /* ---- load fa (old), compute fb_out = fa - HALF(s3), fa_new = fa + s3 ---- */
    esp.vld.128.ip      q0, a0, 16
    esp.vld.128.ip      q1, a0, 16
    esp.vunzip.32       q0, q1          /* q0=fa_r, q1=fa_i */
    addi    a0, a0, -32                 /* reset fa base for store */
    addi    s1, s0, S1R
    esp.vld.128.ip      q2, s1, 0       /* s3r */
    addi    s1, s0, S1I
    esp.vld.128.ip      q3, s1, 0       /* s3i */
    li      s1, 1
    esp.movx.w.sar      s1
    esp.orq             q4, q2, q2
    esp.vsr.s32         q4, q4          /* s3r >> 1 */
    esp.orq             q5, q3, q3
    esp.vsr.s32         q5, q5          /* s3i >> 1 */
    esp.vsub.s32        q6, q0, q4      /* fb_out.r = fa.r - HALF(s3.r) */
    esp.vsub.s32        q7, q1, q5      /* fb_out.i = fa.i - HALF(s3.i) */
    esp.vadd.s32        q0, q0, q2      /* fa_new.r = fa.r + s3.r */
    esp.vadd.s32        q1, q1, q3      /* fa_new.i = fa.i + s3.i */
    esp.vzip.32         q0, q1          /* interleave -> [r,i,r,i] x2 */
    esp.vst.128.ip      q0, a0, 16
    esp.vst.128.ip      q1, a0, 16

    /* ---- fc = (fb_out.r + s0i', fb_out.i - s0r'); fb = (fb_out.r - s0i', fb_out.i + s0r') ---- */
    addi    s1, s0, S2R
    esp.vld.128.ip      q2, s1, 0       /* s0r' */
    addi    s1, s0, S2I
    esp.vld.128.ip      q3, s1, 0       /* s0i' */
    esp.vadd.s32        q4, q6, q3      /* fc.r = fb_out.r + s0i' */
    esp.vsub.s32        q5, q7, q2      /* fc.i = fb_out.i - s0r' */
    esp.vzip.32         q4, q5
    addi    a2, a2, -32                 /* reset fc base (LOADSPLIT advanced it) */
    esp.vst.128.ip      q4, a2, 16
    esp.vst.128.ip      q5, a2, 16
    esp.vsub.s32        q6, q6, q3      /* fb.r = fb_out.r - s0i' */
    esp.vadd.s32        q7, q7, q2      /* fb.i = fb_out.i + s0r' */
    esp.vzip.32         q6, q7
    addi    a1, a1, -32                 /* reset fb base */
    esp.vst.128.ip      q6, a1, 16
    esp.vst.128.ip      q7, a1, 16

    lw      s0, FRAME-8(sp)
    lw      s1, FRAME-16(sp)
    addi    sp, sp, FRAME
    ret

    .size opus_p4_bfly3_4, .-opus_p4_bfly3_4

#endif
