/* Heuristic barcode-region localization (pure C, host-testable).
 *
 * A 1-D barcode is a patch whose luminance gradients all point the
 * SAME way (perpendicular to the bars) — at any rotation. Per 64x64
 * block (sampled every 4th px) we accumulate the structure tensor
 * (Sxx, Syy, Sxy); blocks with high energy AND high coherence
 * (single dominant gradient direction) are barcode-ish. The strongest
 * block seeds a cluster of like-oriented neighbors; its bbox, center
 * and mean gradient direction theta come back to the caller, which
 * samples scanlines ALONG theta (bc_sample_line) — so tilt and
 * portrait/landscape orientation stop mattering.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int found;
    int x0, y0, x1, y1; /* frame-pixel bbox of the block cluster */
    int cx, cy;         /* region center, frame px */
    int theta;          /* gradient direction (perpendicular to the
                           bars), degrees 0..179; scan along this */
    int vertical_bars;  /* coarse: theta closer to horizontal axis */
} bc_region_t;

/* Analyze a DOWNSCALED (anti-aliased) frame — on the device the PPA
 * produces a half-res copy in hardware; sharp full-res input aliases
 * the fine bar pattern into bogus diagonal orientations. All output
 * coordinates are multiplied by coord_scale (2 for a half-res input)
 * so they land in full-frame pixels. */
int bc_locate(const uint16_t *rgb565, int w, int h, int coord_scale,
              bc_region_t *out);

/* Sample one scanline of luma through (cx + v*offset) along direction
 * theta (degrees), half_len px each way, with +-1px binning along the
 * bar direction v. Writes up to max bytes into out; returns the count.
 * Coordinates are clamped at the frame edges. */
int bc_sample_line(const uint16_t *rgb565, int w, int h, int cx, int cy,
                   int theta, int offset_v, int half_len, uint8_t *out,
                   int max);

/* Staged sampling (scanline-opt S2): the angled lines scatter-read the
 * PSRAM frame one cache line per sample; staging copies the sampled
 * extent ONCE (sequential read) into an 8-bit luma buffer and the
 * lines then sample that. bc_sample_line_l8 is bit-identical to
 * bc_sample_line (same fixed-point walk, same frame-edge clamping —
 * fw/fh keep the clamp parity) as long as every frame-clamped sample
 * point lands inside the staged rect; it returns -1 when one does not
 * (caller falls back to the direct path). */
typedef struct {
    const uint8_t *buf;
    int x0, y0; /* stage origin, frame px (after clipping) */
    int w, h;   /* staged size */
    int fw, fh; /* frame size (clamp parity with bc_sample_line) */
} bc_stage_t;

/* Convert the frame rect [x0,x1) x [y0,y1) (clipped to the frame) into
 * buf as 8-bit luma. Returns 0 when the clipped rect is empty or needs
 * more than cap bytes (caller falls back), else fills *st and returns 1. */
int bc_stage_region(const uint16_t *rgb565, int w, int h, int x0, int y0,
                    int x1, int y1, uint8_t *buf, int cap, bc_stage_t *st);

int bc_sample_line_l8(const bc_stage_t *st, int cx, int cy, int theta,
                      int offset_v, int half_len, uint8_t *out, int max);

/* Structure tensor of one 32x32 block (30x30 interior central diffs of
 * the 6-bit green luma): out[0..2] = Sxx, Syy, Sxy. base points at the
 * block's top-left sample, stride_px is the image width in pixels. Pure
 * C; on the device a PIE-assembly variant may be used after a boot
 * self-check confirms it matches this bit-for-bit (see bc_tensor_impl). */
void bc_tensor_block(const uint16_t *base, int stride_px, int32_t out[3]);

/* Which tensor implementation bc_locate() is using:
 *   "pie"                 - PIE assembly (self-check passed)
 *   "c-fallback(mismatch)"- PIE disabled, self-check found a mismatch
 *   "c"                   - plain C (host build, or non-P4 target)
 * Stable string, safe to append to a status line. */
const char *bc_tensor_impl(void);

#ifdef __cplusplus
}
#endif
