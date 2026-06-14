/*
 * ESP32-P4 PIE kf_bfly3 inner kernel: 8 complex (k values) per call.
 *
 * void opus_p4_bfly3_8(int32_t *fa, int32_t *fb, int32_t *fc,
 *                      const int16_t *tw1, const int16_t *tw2, int16_t epi3);
 *
 * Same contract/math as opus_p4_bfly3_4 but 8-wide: full SIMD lane use (each
 * vmul.s32.s16xs16 produces 8 valid products in 2 dst qregs), so the marshaling
 * is cleaner than the 4-wide version -- vunzip.16 of the two int32 qregs yields
 * clean 8-lane lo/hi splits, no self-copy. Each complex vector (r or i, 8 int32)
 * is 2 qregs; the 8 QRs can't hold fb/fc/s1/s2 at once, so splits and the
 * s1/s2/s3/s0 vectors live in a stack scratch frame and products are computed
 * one at a time. Relaxed signed low-half split (matches opus_p4_bfly3_c_rx,
 * ~-75 dB) like comb/denorm.
 *
 * ABI: a0=fa,a1=fb,a2=fc,a3=tw1,a4=tw2,a5=epi3. PIE base GPRs x8..x15: s0 (x8)
 * = scratch base, s1 (x9) = transient address / SAR. a0/a1/a2 post-incremented
 * by their loads (+64) and reset before stores.
 */

#include "sdkconfig.h"

#if defined(__riscv) && defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4

    /* scratch slot offsets from s0. s16 split vectors are 16B (8 s16); int32
     * result vectors are 32B (8 int32). */
    .equ  RLO, 0       /* data real lo-s16 (also reused: fa_r / fa_new_r 32B) */
    .equ  RHI, 16
    .equ  ILO, 32      /* data imag lo-s16 (also reused: fa_i / fa_new_i 32B) */
    .equ  IHI, 48
    .equ  TR,  64      /* twiddle real (8 s16); also epi3 scalar stash */
    .equ  TI,  80
    .equ  S1R, 96      /* s1.r / then s3.r / then fb_out.r (32B) */
    .equ  S1I, 128
    .equ  S2R, 160     /* s2.r / then s0.r / then s0.r' (32B) */
    .equ  S2I, 192
    .equ  FRAME, 256

    .text
    .align  2
    .global opus_p4_bfly3_8
    .type   opus_p4_bfly3_8,@function

    /* 1-qreg load/store from slot \off */
    .macro LD1 qq, off
    addi    s1, s0, \off
    esp.vld.128.ip      \qq, s1, 0
    .endm
    .macro ST1 qq, off
    addi    s1, s0, \off
    esp.vst.128.ip      \qq, s1, 0
    .endm
    /* 2-qreg (lo=elem0-3, hi=elem4-7) load/store from slot \off */
    .macro LD2 qlo, qhi, off
    addi    s1, s0, \off
    esp.vld.128.ip      \qlo, s1, 16
    esp.vld.128.ip      \qhi, s1, 16
    .endm
    .macro ST2 qlo, qhi, off
    addi    s1, s0, \off
    esp.vst.128.ip      \qlo, s1, 16
    esp.vst.128.ip      \qhi, s1, 16
    .endm

    /* M(t,d) = (t*d_hi)*2 + (t*d_lo)>>15 over 8 lanes -> (\mlo,\mhi).
     * \t,\dhi,\dlo are 1 qreg of 8 s16; \tlo,\thi are scratch (plo pair). */
    .macro PROD8 mlo, mhi, t, dhi, dlo, tlo, thi
    li      s1, 0
    esp.movx.w.sar      s1
    esp.vmul.s32.s16xs16 \mlo, \mhi, \t, \dhi
    esp.vadd.s32        \mlo, \mlo, \mlo
    esp.vadd.s32        \mhi, \mhi, \mhi
    li      s1, 15
    esp.movx.w.sar      s1
    esp.vmul.s32.s16xs16 \tlo, \thi, \t, \dlo
    esp.vadd.s32        \mlo, \mlo, \tlo
    esp.vadd.s32        \mhi, \mhi, \thi
    .endm

    /* Load 8 complex int32 from \ptr (post-inc 64); store r_lo,r_hi,i_lo,i_hi. */
    .macro LOADSPLIT8 ptr
    esp.vld.128.ip      q0, \ptr, 16
    esp.vld.128.ip      q1, \ptr, 16
    esp.vld.128.ip      q2, \ptr, 16
    esp.vld.128.ip      q3, \ptr, 16
    esp.vunzip.32       q0, q1          /* q0=r[0..3], q1=i[0..3] */
    esp.vunzip.32       q2, q3          /* q2=r[4..7], q3=i[4..7] */
    esp.vunzip.16       q0, q2          /* q0=r_lo(8), q2=r_hi(8) */
    ST1     q0, RLO
    ST1     q2, RHI
    esp.vunzip.16       q1, q3          /* q1=i_lo(8), q3=i_hi(8) */
    ST1     q1, ILO
    ST1     q3, IHI
    .endm

    /* Load 8 complex int16 twiddles from \ptr (post-inc 32); store TR/TI. */
    .macro TWSPLIT8 ptr
    esp.vld.128.ip      q0, \ptr, 16
    esp.vld.128.ip      q1, \ptr, 16
    esp.vunzip.16       q0, q1          /* q0=tr(8 even), q1=ti(8 odd) */
    ST1     q0, TR
    ST1     q1, TI
    .endm

    /* C_MUL of data in RLO/RHI/ILO/IHI by twiddle TR/TI -> s.r@\sr, s.i@\si. */
    .macro CMUL8 sr, si
    /* A = M(tr,dr) -> \sr (temp) */
    LD1     q5, TR
    LD1     q6, RHI
    LD1     q7, RLO
    PROD8   q0, q1, q5, q6, q7, q2, q3
    ST2     q0, q1, \sr
    /* B = M(ti,di) */
    LD1     q5, TI
    LD1     q6, IHI
    LD1     q7, ILO
    PROD8   q0, q1, q5, q6, q7, q2, q3
    /* s.r = A - B */
    LD2     q2, q3, \sr
    esp.vsub.s32        q2, q2, q0
    esp.vsub.s32        q3, q3, q1
    ST2     q2, q3, \sr
    /* C = M(tr,di) -> \si (temp) */
    LD1     q5, TR
    LD1     q6, IHI
    LD1     q7, ILO
    PROD8   q0, q1, q5, q6, q7, q2, q3
    ST2     q0, q1, \si
    /* D = M(ti,dr) */
    LD1     q5, TI
    LD1     q6, RHI
    LD1     q7, RLO
    PROD8   q0, q1, q5, q6, q7, q2, q3
    /* s.i = C + D */
    LD2     q2, q3, \si
    esp.vadd.s32        q2, q2, q0
    esp.vadd.s32        q3, q3, q1
    ST2     q2, q3, \si
    .endm

opus_p4_bfly3_8:
    addi    sp, sp, -FRAME
    sw      s0, FRAME-8(sp)
    sw      s1, FRAME-16(sp)
    addi    s0, sp, 0

    LOADSPLIT8 a1                       /* fb */
    TWSPLIT8 a3                         /* tw1 */
    CMUL8   S1R, S1I

    LOADSPLIT8 a2                       /* fc */
    TWSPLIT8 a4                         /* tw2 */
    CMUL8   S2R, S2I

    /* s3 = s1+s2 (-> S1*), s0 = s1-s2 (-> S2*) */
    LD2     q0, q1, S1R
    LD2     q2, q3, S2R
    esp.vadd.s32        q4, q0, q2
    esp.vadd.s32        q5, q1, q3
    esp.vsub.s32        q6, q0, q2
    esp.vsub.s32        q7, q1, q3
    ST2     q4, q5, S1R                 /* s3r */
    ST2     q6, q7, S2R                 /* s0r */
    LD2     q0, q1, S1I
    LD2     q2, q3, S2I
    esp.vadd.s32        q4, q0, q2
    esp.vadd.s32        q5, q1, q3
    esp.vsub.s32        q6, q0, q2
    esp.vsub.s32        q7, q1, q3
    ST2     q4, q5, S1I                 /* s3i */
    ST2     q6, q7, S2I                 /* s0i */

    /* s0 *= epi3 (broadcast) */
    sh      a5, TR(s0)
    addi    s1, s0, TR
    esp.vldbc.16.ip     q7, s1, 0       /* q7 = epi3 (8 lanes) */
    LD2     q0, q1, S2R                 /* s0r */
    esp.vunzip.16       q0, q1          /* q0=s0r_lo, q1=s0r_hi */
    PROD8   q2, q3, q7, q1, q0, q4, q5  /* M(epi3,s0r) */
    ST2     q2, q3, S2R                 /* s0r' */
    LD2     q0, q1, S2I                 /* s0i */
    esp.vunzip.16       q0, q1
    PROD8   q2, q3, q7, q1, q0, q4, q5
    ST2     q2, q3, S2I                 /* s0i' */

    /* load fa (old) */
    esp.vld.128.ip      q0, a0, 16
    esp.vld.128.ip      q1, a0, 16
    esp.vld.128.ip      q2, a0, 16
    esp.vld.128.ip      q3, a0, 16
    esp.vunzip.32       q0, q1          /* q0=fa_r[0..3], q1=fa_i[0..3] */
    esp.vunzip.32       q2, q3          /* q2=fa_r[4..7], q3=fa_i[4..7] */
    addi    a0, a0, -64
    ST2     q0, q2, RLO                 /* fa_r (lo,hi) */
    ST2     q1, q3, ILO                 /* fa_i */

    /* real: fb_out.r = fa.r - HALF(s3.r); fa_new.r = fa.r + s3.r */
    LD2     q0, q1, S1R                 /* s3r */
    li      s1, 1
    esp.movx.w.sar      s1
    esp.orq             q2, q0, q0
    esp.vsr.s32         q2, q2          /* s3r>>1 lo */
    esp.orq             q3, q1, q1
    esp.vsr.s32         q3, q3          /* s3r>>1 hi */
    LD2     q4, q5, RLO                 /* fa_r */
    esp.vsub.s32        q6, q4, q2      /* fb_out.r lo */
    esp.vsub.s32        q7, q5, q3      /* fb_out.r hi */
    esp.vadd.s32        q4, q4, q0      /* fa_new.r lo */
    esp.vadd.s32        q5, q5, q1      /* fa_new.r hi */
    ST2     q4, q5, RLO                 /* fa_new.r */
    ST2     q6, q7, S1R                 /* fb_out.r */

    /* imag: fb_out.i, fa_new.i */
    LD2     q0, q1, S1I                 /* s3i */
    li      s1, 1
    esp.movx.w.sar      s1
    esp.orq             q2, q0, q0
    esp.vsr.s32         q2, q2
    esp.orq             q3, q1, q1
    esp.vsr.s32         q3, q3
    LD2     q4, q5, ILO                 /* fa_i */
    esp.vsub.s32        q6, q4, q2      /* fb_out.i lo */
    esp.vsub.s32        q7, q5, q3
    esp.vadd.s32        q4, q4, q0      /* fa_new.i */
    esp.vadd.s32        q5, q5, q1
    ST2     q4, q5, ILO                 /* fa_new.i */
    ST2     q6, q7, S1I                 /* fb_out.i */

    /* store fa_new (interleave r/i) */
    LD2     q0, q1, RLO                 /* fa_new.r (q0=0-3,q1=4-7) */
    LD2     q2, q3, ILO                 /* fa_new.i */
    esp.vzip.32         q0, q2          /* q0=[r0,i0,r1,i1], q2=[r2,i2,r3,i3] */
    esp.vzip.32         q1, q3          /* q1=[r4,i4,r5,i5], q3=[r6,i6,r7,i7] */
    esp.vst.128.ip      q0, a0, 16
    esp.vst.128.ip      q2, a0, 16
    esp.vst.128.ip      q1, a0, 16
    esp.vst.128.ip      q3, a0, 16

    /* fc = (fb_out.r + s0i', fb_out.i - s0r') */
    LD2     q0, q1, S1R                 /* fb_out.r */
    LD2     q2, q3, S2I                 /* s0i' */
    esp.vadd.s32        q4, q0, q2      /* fc.r lo */
    esp.vadd.s32        q5, q1, q3      /* fc.r hi */
    LD2     q2, q3, S1I                 /* fb_out.i */
    LD2     q0, q1, S2R                 /* s0r' */
    esp.vsub.s32        q6, q2, q0      /* fc.i lo */
    esp.vsub.s32        q7, q3, q1      /* fc.i hi */
    esp.vzip.32         q4, q6
    esp.vzip.32         q5, q7
    addi    a2, a2, -64
    esp.vst.128.ip      q4, a2, 16
    esp.vst.128.ip      q6, a2, 16
    esp.vst.128.ip      q5, a2, 16
    esp.vst.128.ip      q7, a2, 16

    /* fb = (fb_out.r - s0i', fb_out.i + s0r') */
    LD2     q0, q1, S1R                 /* fb_out.r */
    LD2     q2, q3, S2I                 /* s0i' */
    esp.vsub.s32        q4, q0, q2
    esp.vsub.s32        q5, q1, q3
    LD2     q2, q3, S1I                 /* fb_out.i */
    LD2     q0, q1, S2R                 /* s0r' */
    esp.vadd.s32        q6, q2, q0
    esp.vadd.s32        q7, q3, q1
    esp.vzip.32         q4, q6
    esp.vzip.32         q5, q7
    addi    a1, a1, -64
    esp.vst.128.ip      q4, a1, 16
    esp.vst.128.ip      q6, a1, 16
    esp.vst.128.ip      q5, a1, 16
    esp.vst.128.ip      q7, a1, 16

    lw      s0, FRAME-8(sp)
    lw      s1, FRAME-16(sp)
    addi    sp, sp, FRAME
    ret

    .size opus_p4_bfly3_8, .-opus_p4_bfly3_8

#endif
