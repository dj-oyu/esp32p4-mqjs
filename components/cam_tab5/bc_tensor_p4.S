/* ESP32-P4 PIE (xesppie) hand-fused structure-tensor kernel for the
 * barcode localizer. Computes, for a 32x32-px RGB565 block, the 30x30
 * interior central-difference structure tensor:
 *
 *   l  = (v >> 5) & 0x3F           (6-bit green field = luma_fast)
 *   gx = l[y][x+1] - l[y][x-1]
 *   gy = l[y+1][x] - l[y-1][x]
 *   out[0] = Sxx = sum gx*gx,  out[1] = Syy = sum gy*gy,
 *   out[2] = Sxy = sum gx*gy   (int32; |sum| <= ~3.6M, safe)
 *
 * Output matches the C reference (bc_tensor_block) BIT-FOR-BIT; the boot
 * self-check in bc_locate.c verifies that on real silicon.
 *
 * --- design notes (resolved §6 of docs/pie-tensor-asm-plan.md) -------
 * G6 extraction WITHOUT a lane-shift instruction: mask each 16-bit lane
 * with 0x07E0 (esp.andq against a splatted constant). That leaves the
 * green field as G<<5 = 32*G, so every gradient is 32x and every product
 * is 1024x. We accumulate the 1024x sums in XACC and arithmetic-right-
 * shift the extracted 32-bit result by 10 (esp.srs.s.xacc, shift=10) to
 * undo it EXACTLY (the true sum is an integer; the scale is an exact
 * power of two, so >>10 loses nothing). No per-lane vector shift needed.
 *
 * XACC, not QACC: esp.vmulas.s16.xacc reduce-sums all 8 lane products
 * into the scalar 40-bit XACC each MAC — the accumulator esp-dsp's
 * dsps_dotprod_s16_arp4 uses and the one esp.srs.s.xacc extracts from.
 * (The first cut of this kernel accumulated into QACC and extracted
 * XACC; the boot self-check caught the mismatch on real silicon.)
 * XACC is single, so the three sums cannot share it: three passes over
 * the block's rows, zeroing XACC before each and extracting after. The
 * block is 2 KB and stays L2-resident across the passes, so the extra
 * memory reads are cheap; gradients are recomputed (one esp.vsub each)
 * rather than spilled to memory.
 *
 * Unaligned loads: the +-1px windows live at even but non-16B byte
 * offsets, so unaligned PIE access is enabled once via the cfg register
 * (set bit 1), exactly as esp-dsp's dsps_dotprod_s16_arp4 does.
 *
 * Interior masking: the 30 interior columns (sx = 1..30) are processed
 * as four 8-lane vectors covering sx = 1..32; the unused lanes (sx =
 * 31,32 in the 4th chunk) are forced to zero by ANDing a per-chunk
 * validity mask into the gradient before the MAC (a zero gradient adds
 * nothing). The mask vectors come from a 4-entry table read with a
 * pointer-increment load, so the column loop body is BRANCH-FREE and can
 * use esp.lp.setupi (single-level zero-overhead hardware loop).
 *
 * Xhwlp is one level deep on purpose: esp.lp.setupi wraps the 4 column
 * chunks; the 30-row and 3-pass loops use ordinary RISC-V branches. This
 * avoids the hardware-loop nesting question (esp-dsp itself fell back to
 * a manual branch loop rather than nest esp.lp.setup).
 */

#if defined(__riscv) && defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4

    .data
    .align 4
/* 0x07E0 splat: isolates the 6-bit green field (kept as G<<5). */
.Lmask_g6:
    .short 0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0,0x07E0
/* Per-chunk column-validity masks, 4 consecutive 16B vectors. Chunks
 * 0..2 (sx 1..24) are fully valid; chunk 3 (sx 25..32) zeroes the top
 * two lanes (sx 31,32 are outside the 1..30 interior). Read with a
 * pointer-increment load inside the hardware loop -> no branch. */
.Lvalid:
    .short -1,-1,-1,-1,-1,-1,-1,-1   /* chunk 0 */
    .short -1,-1,-1,-1,-1,-1,-1,-1   /* chunk 1 */
    .short -1,-1,-1,-1,-1,-1,-1,-1   /* chunk 2 */
    .short -1,-1,-1,-1,-1,-1, 0, 0   /* chunk 3 */

    .text
    .align 4
    .global bc_tensor_block_p4
    .type   bc_tensor_block_p4,@function

/* void bc_tensor_block_p4(const uint16_t *base, int stride_px,
 *                         int32_t out[3])
 *   a0 = base, a1 = stride_px, a2 = out
 *
 * Persistent registers across the whole call:
 *   a0  base byte address of row 0      a1 stride_px
 *   a2  out[3]
 *   t0  row byte stride = stride_px*2
 *   q0  G6 mask (0x07E0 splat)
 * Per-pass / per-row scratch:
 *   a3  pass index (0=Sxx,1=Syy,2=Sxy)
 *   t1  sy counter
 *   t2  byte addr of row sy
 *   t3  left/up window pointer    t4 right/down window pointer
 *   t5  mask-table pointer / scratch
 *   t6  cfg / shift scratch
 *   a4,a5 gy window pointers (Sxy pass) / scratch
 *   q1  per-chunk validity mask
 *   q2  left/up luma            q3 right/down luma
 *   q4  gx                      q5 gy (Sxy pass)
 */
bc_tensor_block_p4:
    /* enable unaligned PIE loads (cfg bit 1) */
    esp.movx.r.cfg  t6
    ori     t6, t6, 2
    esp.movx.w.cfg  t6

    slli    t0, a1, 1                /* row stride in bytes */
    la      t5, .Lmask_g6
    esp.vld.128.ip  q0, t5, 0        /* q0 = G6 mask */

    /* =============================== Sxx (pass 0) =============== */
    li      a3, 0
    esp.zero.xacc
    li      t1, 1
    add     t2, a0, t0               /* row 1 */
.Lrow_xx:
    mv      t3, t2                   /* left  = col 0 */
    addi    t4, t2, 4                /* right = col 2 */
    la      t5, .Lvalid
    /* NB: the hwlp end label is INCLUSIVE — it must sit ON the last
       instruction of the body (esp-dsp's .dotprod_loop does the same);
       a label placed after the body pulls the next instruction in. */
    esp.lp.setupi  0, 4, .Lmac_xx
        esp.vld.128.ip  q2, t3, 16
        esp.vld.128.ip  q3, t4, 16
        esp.vld.128.ip  q1, t5, 16   /* per-chunk mask, auto-advance */
        esp.andq        q2, q2, q0
        esp.andq        q3, q3, q0
        esp.vsub.s16    q4, q3, q2   /* gx*32 */
        esp.andq        q4, q4, q1
.Lmac_xx:
        esp.vmulas.s16.xacc  q4, q4  /* += gx*gx (1024x) */
    addi    t1, t1, 1
    add     t2, t2, t0
    li      t5, 31
    blt     t1, t5, .Lrow_xx
    li      t6, 10
    esp.srs.s.xacc  t5, t6
    sw      t5, 0(a2)                /* out[0] = Sxx */

    /* =============================== Syy (pass 1) =============== */
    esp.zero.xacc
    li      t1, 1
    add     t2, a0, t0               /* row 1 */
.Lrow_yy:
    sub     t3, t2, t0               /* up   = row sy-1 */
    add     t4, t2, t0               /* down = row sy+1 */
    addi    t3, t3, 2                /* col 1 */
    addi    t4, t4, 2
    la      t5, .Lvalid
    esp.lp.setupi  0, 4, .Lmac_yy
        esp.vld.128.ip  q2, t3, 16
        esp.vld.128.ip  q3, t4, 16
        esp.vld.128.ip  q1, t5, 16
        esp.andq        q2, q2, q0
        esp.andq        q3, q3, q0
        esp.vsub.s16    q4, q3, q2   /* gy*32 */
        esp.andq        q4, q4, q1
.Lmac_yy:
        esp.vmulas.s16.xacc  q4, q4  /* += gy*gy (1024x) */
    addi    t1, t1, 1
    add     t2, t2, t0
    li      t5, 31
    blt     t1, t5, .Lrow_yy
    li      t6, 10
    esp.srs.s.xacc  t5, t6
    sw      t5, 4(a2)                /* out[1] = Syy */

    /* =============================== Sxy (pass 2) =============== */
    esp.zero.xacc
    li      t1, 1
    add     t2, a0, t0               /* row 1 */
.Lrow_xy:
    mv      t3, t2                   /* gx left  = col 0 */
    addi    t4, t2, 4                /* gx right = col 2 */
    sub     a4, t2, t0
    addi    a4, a4, 2                /* gy up   = row sy-1, col 1 */
    add     a5, t2, t0
    addi    a5, a5, 2                /* gy down = row sy+1, col 1 */
    la      t5, .Lvalid
    esp.lp.setupi  0, 4, .Lmac_xy
        esp.vld.128.ip  q2, t3, 16   /* gx left  */
        esp.vld.128.ip  q3, t4, 16   /* gx right */
        esp.andq        q2, q2, q0
        esp.andq        q3, q3, q0
        esp.vsub.s16    q4, q3, q2   /* gx*32 */
        esp.vld.128.ip  q2, a4, 16   /* gy up    (reuse q2) */
        esp.vld.128.ip  q3, a5, 16   /* gy down  (reuse q3) */
        esp.andq        q2, q2, q0
        esp.andq        q3, q3, q0
        esp.vsub.s16    q5, q3, q2   /* gy*32 */
        esp.vld.128.ip  q1, t5, 16   /* per-chunk mask */
        esp.andq        q4, q4, q1
        esp.andq        q5, q5, q1
.Lmac_xy:
        esp.vmulas.s16.xacc  q4, q5  /* += gx*gy (1024x) */
    addi    t1, t1, 1
    add     t2, t2, t0
    li      t5, 31
    blt     t1, t5, .Lrow_xy
    li      t6, 10
    esp.srs.s.xacc  t5, t6
    sw      t5, 8(a2)                /* out[2] = Sxy */

    ret

    .size   bc_tensor_block_p4, .-bc_tensor_block_p4

#endif /* CONFIG_IDF_TARGET_ESP32P4 */
#endif /* __riscv && ESP_PLATFORM */
