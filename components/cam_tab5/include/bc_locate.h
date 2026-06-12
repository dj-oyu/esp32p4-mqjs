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

#ifdef __cplusplus
}
#endif
