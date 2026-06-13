/* Barcode-region localization + angled scanline sampling.
 * See include/bc_locate.h. */
#include "bc_locate.h"

#include <math.h>
#include <string.h>

/* NOTE on PIE: the tensor sums were tried as three esp-dsp dot
 * products (dsps_dotprod_s16_arp4, PIE-accelerated, 16B-aligned
 * buffers). Measured ON DEVICE it was ~2x SLOWER than this fused
 * scalar loop (loc 22-23ms vs 11-20ms): 324 library calls per frame
 * plus materializing the gradient arrays (write+reread) cost more
 * than the vector MACs saved. A win here would need hand-fused PIE
 * assembly (load+convert+diff+MAC in one pass) — not worth it while
 * the PPA pixel rate dominates the pipeline. */

#define BLK 32        /* block size in ANALYSIS-image px (x2 in frame) */
#define GRID BLK      /* every pixel of the downscaled image */
#define MAX_BX 32     /* up to 1024px-wide analysis images */
#define MAX_BY 24

/* energy floor: a barcode block (even in shadow, ~40 gray levels of
 * contrast) accumulates squared 6-bit-luma gradients in the 1e4-1e5
 * range; sensor noise and smooth lighting stay an order of magnitude
 * lower. (Was 60000 with 8-bit luma; 6-bit products are 16x smaller.) */
#define E_MIN 3750
/* coherence floor 0.6: (Sxx-Syy)^2 + 4Sxy^2 > (0.6 * energy)^2 */
#define COH_NUM 36
#define COH_DEN 100
/* cluster joins blocks whose orientation agrees within this (deg) */
#define TH_JOIN 20

static inline int luma(uint16_t v)
{
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return (77 * (r << 3) + 150 * (g << 2) + 29 * (b << 3)) >> 8;
}

/* tensor-only luma: the raw 6-bit green field (59% of luminance, and
 * barcodes are achromatic anyway) — one shift+mask instead of three
 * multiplies, in the hottest per-pixel loop we own */
static inline int luma_fast(uint16_t v)
{
    return (v >> 5) & 0x3F;
}

/* Per-block structure tensor over the 30x30 interior of a 32x32 block.
 * base/stride_px address the block's top-left luma sample (RGB565).
 * out[0..2] = Sxx, Syy, Sxy with 6-bit-green luma (luma_fast).
 *
 * This is the host path AND the device C fallback. The PIE assembly
 * kernel (bc_tensor_p4.S) must match this bit-for-bit; the boot
 * self-check (below) verifies that on the real silicon. */
void bc_tensor_block(const uint16_t *base, int stride_px, int32_t out[3])
{
    int l[GRID][GRID];
    for (int sy = 0; sy < GRID; sy++) {
        const uint16_t *row = base + sy * stride_px;
        for (int sx = 0; sx < GRID; sx++)
            l[sy][sx] = luma_fast(row[sx]);
    }
    int32_t sxx = 0, syy = 0, sxy = 0;
    for (int sy = 1; sy < GRID - 1; sy++) {
        for (int sx = 1; sx < GRID - 1; sx++) {
            int gx = l[sy][sx + 1] - l[sy][sx - 1];
            int gy = l[sy + 1][sx] - l[sy - 1][sx];
            sxx += gx * gx;
            syy += gy * gy;
            sxy += gx * gy;
        }
    }
    out[0] = sxx;
    out[1] = syy;
    out[2] = sxy;
}

/* ---- tensor implementation dispatch + boot self-check -------------- */

#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4
/* PIE assembly variant (bc_tensor_p4.S). Same contract as
 * bc_tensor_block; loads RGB565 unaligned, masks the 6-bit green field
 * (kept as G<<5), accumulates 1024x-scaled sums in QACC and rescales by
 * >>10 before returning, so the output matches the C path exactly. */
extern void bc_tensor_block_p4(const uint16_t *base, int stride_px,
                               int32_t out[3]);

/* 0 = not yet checked, 1 = PIE OK, 2 = mismatch -> permanent C fallback */
static int s_tensor_mode;

/* xorshift32 PRNG: fixed seed -> identical synthetic blocks every boot,
 * so the self-check is fully deterministic (no serial console needed). */
static uint32_t bc_xs32(uint32_t *st)
{
    uint32_t x = *st;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *st = x;
}

/* Fill a 32x32 (16B-aligned, stride 32) test block from the PRNG, run
 * both tensor paths on 8 such blocks, and require all three sums to
 * agree. On ANY mismatch latch the C fallback permanently. Runs once,
 * on the first bc_locate() call on device. */
static void bc_tensor_selfcheck(void)
{
    static uint16_t blk[32 * 32] __attribute__((aligned(16)));
    uint32_t st = 0x1b6f3a9du; /* fixed seed */
    int mismatch = 0;
    for (int n = 0; n < 8 && !mismatch; n++) {
        for (int i = 0; i < 32 * 32; i++)
            blk[i] = (uint16_t)bc_xs32(&st);
        int32_t c[3], p[3];
        bc_tensor_block(blk, 32, c);
        bc_tensor_block_p4(blk, 32, p);
        if (c[0] != p[0] || c[1] != p[1] || c[2] != p[2])
            mismatch = 1;
    }
    s_tensor_mode = mismatch ? 2 : 1;
}

static void bc_tensor_dispatch(const uint16_t *base, int stride_px,
                               int32_t out[3])
{
    if (s_tensor_mode == 0)
        bc_tensor_selfcheck();
    if (s_tensor_mode == 1)
        bc_tensor_block_p4(base, stride_px, out);
    else
        bc_tensor_block(base, stride_px, out);
}

const char *bc_tensor_impl(void)
{
    return s_tensor_mode == 1 ? "pie"
         : s_tensor_mode == 2 ? "c-fallback(mismatch)"
         : "c"; /* not yet self-checked (no block processed) */
}
#else  /* host build / non-P4: plain C only */
static void bc_tensor_dispatch(const uint16_t *base, int stride_px,
                               int32_t out[3])
{
    bc_tensor_block(base, stride_px, out);
}

const char *bc_tensor_impl(void)
{
    return "c";
}
#endif

/* circular distance for orientations with period 180 */
static int th_dist(int a, int b)
{
    int d = a - b;
    while (d > 90)
        d -= 180;
    while (d < -90)
        d += 180;
    return d < 0 ? -d : d;
}

int bc_locate(const uint16_t *rgb565, int w, int h, int coord_scale,
              bc_region_t *out)
{
    memset(out, 0, sizeof *out);
    out->theta = -1;
    int nbx = w / BLK, nby = h / BLK;
    if (nbx < 2 || nby < 2)
        return 0;
    if (nbx > MAX_BX)
        nbx = MAX_BX;
    if (nby > MAX_BY)
        nby = MAX_BY;

    static int32_t energy[MAX_BY][MAX_BX];
    static int16_t theta[MAX_BY][MAX_BX]; /* deg 0..179; -1 = not ok */

    for (int by = 0; by < nby; by++) {
        for (int bx = 0; bx < nbx; bx++) {
            const uint16_t *base = rgb565 + (by * BLK) * w + bx * BLK;
            /* central differences, both centered on the SAME sample —
               forward diffs put gx and gy half a step apart, which
               biases Sxy toward +45° and mirrors angles beyond 90°.
               Fused load/convert/diff/MAC loop: everything stays in
               registers (see the PIE note at the top of the file). */
            int32_t s[3];
            bc_tensor_dispatch(base, w, s);
            int32_t sxx = s[0], syy = s[1], sxy = s[2];
            int32_t e = sxx + syy;
            theta[by][bx] = -1;
            energy[by][bx] = e;
            if (e < E_MIN)
                continue;
            int64_t num = (int64_t)(sxx - syy) * (sxx - syy) +
                          (int64_t)(2 * sxy) * (2 * sxy);
            if (num * COH_DEN < (int64_t)e * e * COH_NUM)
                continue; /* gradients point all over: texture, not bars */
            float th = 0.5f * atan2f((float)(2 * sxy), (float)(sxx - syy));
            int deg = (int)lroundf(th * 180.0f / (float)M_PI);
            while (deg < 0)
                deg += 180;
            while (deg >= 180)
                deg -= 180;
            theta[by][bx] = (int16_t)deg;
        }
    }

    /* strongest qualifying block seeds the cluster */
    int32_t best = 0;
    int sx0 = -1, sy0 = -1;
    for (int by = 0; by < nby; by++)
        for (int bx = 0; bx < nbx; bx++)
            if (theta[by][bx] >= 0 && energy[by][bx] > best) {
                best = energy[by][bx];
                sx0 = bx;
                sy0 = by;
            }
    if (sx0 < 0)
        return 0;

    /* BFS over 4-neighbors with agreeing orientation */
    static uint8_t in[MAX_BY][MAX_BX];
    static int16_t qx[MAX_BX * MAX_BY], qy[MAX_BX * MAX_BY];
    memset(in, 0, sizeof in);
    int qh = 0, qt = 0;
    int seed_th = theta[sy0][sx0];
    qx[qt] = (int16_t)sx0;
    qy[qt++] = (int16_t)sy0;
    in[sy0][sx0] = 1;
    int x0 = sx0, x1 = sx0, y0 = sy0, y1 = sy0;
    float c2 = 0, s2 = 0;
    while (qh < qt) {
        int bx = qx[qh], by = qy[qh];
        qh++;
        int t = theta[by][bx];
        float r = t * (float)M_PI / 90.0f; /* 2*theta in radians */
        c2 += cosf(r);
        s2 += sinf(r);
        if (bx < x0) x0 = bx;
        if (bx > x1) x1 = bx;
        if (by < y0) y0 = by;
        if (by > y1) y1 = by;
        static const int dx[4] = { 1, -1, 0, 0 };
        static const int dy[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) {
            int nx = bx + dx[k], ny = by + dy[k];
            if (nx < 0 || nx >= nbx || ny < 0 || ny >= nby || in[ny][nx])
                continue;
            if (theta[ny][nx] < 0 ||
                th_dist(theta[ny][nx], seed_th) > TH_JOIN)
                continue;
            in[ny][nx] = 1;
            qx[qt] = (int16_t)nx;
            qy[qt++] = (int16_t)ny;
        }
    }

    int mean = (int)lroundf(atan2f(s2, c2) * 90.0f / (float)M_PI);
    while (mean < 0)
        mean += 180;
    while (mean >= 180)
        mean -= 180;

    out->found = 1;
    out->theta = mean;
    out->vertical_bars = mean < 45 || mean > 135;
    out->x0 = x0 * BLK * coord_scale;
    out->y0 = y0 * BLK * coord_scale;
    out->x1 = (x1 + 1) * BLK * coord_scale;
    out->y1 = (y1 + 1) * BLK * coord_scale;
    out->cx = (out->x0 + out->x1) / 2;
    out->cy = (out->y0 + out->y1) / 2;
    return 1;
}

int bc_sample_line(const uint16_t *f, int w, int h, int cx, int cy,
                   int theta, int offset_v, int half_len, uint8_t *out,
                   int max)
{
    float th = theta * (float)M_PI / 180.0f;
    float ux = cosf(th), uy = sinf(th);
    float vx = -uy, vy = ux;
    int n = 2 * half_len;
    if (n > max)
        n = max;
    if (n < 1)
        return 0;
    /* float only for setup; the inner loop steps in 16.16 fixed point
       (frame coords < 2048 fit comfortably) */
    int32_t fux = (int32_t)(ux * 65536.0f);
    int32_t fuy = (int32_t)(uy * 65536.0f);
    int32_t bx = (int32_t)(vx * 65536.0f);
    int32_t by = (int32_t)(vy * 65536.0f);
    int32_t x = (int32_t)((cx + vx * offset_v - ux * (n / 2)) * 65536.0f);
    int32_t y = (int32_t)((cy + vy * offset_v - uy * (n / 2)) * 65536.0f);
    for (int i = 0; i < n; i++, x += fux, y += fuy) {
        int acc = 0;
        for (int b = -1; b <= 1; b++) {
            int xi = (x + b * bx + 32768) >> 16;
            int yi = (y + b * by + 32768) >> 16;
            if (xi < 0)
                xi = 0;
            if (xi > w - 1)
                xi = w - 1;
            if (yi < 0)
                yi = 0;
            if (yi > h - 1)
                yi = h - 1;
            acc += luma(f[yi * w + xi]);
        }
        out[i] = (uint8_t)(acc / 3);
    }
    return n;
}

int bc_stage_region(const uint16_t *f, int w, int h, int x0, int y0,
                    int x1, int y1, uint8_t *buf, int cap, bc_stage_t *st)
{
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > w)
        x1 = w;
    if (y1 > h)
        y1 = h;
    int sw = x1 - x0, sh = y1 - y0;
    if (sw <= 0 || sh <= 0 || (int64_t)sw * sh > cap)
        return 0;
    for (int y = 0; y < sh; y++) {
        const uint16_t *src = f + (size_t)(y0 + y) * w + x0;
        uint8_t *dst = buf + (size_t)y * sw;
        for (int x = 0; x < sw; x++)
            dst[x] = (uint8_t)luma(src[x]);
    }
    st->buf = buf;
    st->x0 = x0;
    st->y0 = y0;
    st->w = sw;
    st->h = sh;
    st->fw = w;
    st->fh = h;
    return 1;
}

/* keep this the line-for-line twin of bc_sample_line above: same
   fixed-point walk, same FRAME-edge clamping (st->fw/fh), only the
   pixel fetch differs (staged 8-bit luma instead of RGB565+luma()) */
int bc_sample_line_l8(const bc_stage_t *st, int cx, int cy, int theta,
                      int offset_v, int half_len, uint8_t *out, int max)
{
    float th = theta * (float)M_PI / 180.0f;
    float ux = cosf(th), uy = sinf(th);
    float vx = -uy, vy = ux;
    int n = 2 * half_len;
    if (n > max)
        n = max;
    if (n < 1)
        return 0;
    int32_t fux = (int32_t)(ux * 65536.0f);
    int32_t fuy = (int32_t)(uy * 65536.0f);
    int32_t bx = (int32_t)(vx * 65536.0f);
    int32_t by = (int32_t)(vy * 65536.0f);
    int32_t x = (int32_t)((cx + vx * offset_v - ux * (n / 2)) * 65536.0f);
    int32_t y = (int32_t)((cy + vy * offset_v - uy * (n / 2)) * 65536.0f);
    for (int i = 0; i < n; i++, x += fux, y += fuy) {
        int acc = 0;
        for (int b = -1; b <= 1; b++) {
            int xi = (x + b * bx + 32768) >> 16;
            int yi = (y + b * by + 32768) >> 16;
            if (xi < 0)
                xi = 0;
            if (xi > st->fw - 1)
                xi = st->fw - 1;
            if (yi < 0)
                yi = 0;
            if (yi > st->fh - 1)
                yi = st->fh - 1;
            xi -= st->x0;
            yi -= st->y0;
            if (xi < 0 || xi >= st->w || yi < 0 || yi >= st->h)
                return -1; /* stage too small: caller goes direct */
            acc += st->buf[(size_t)yi * st->w + xi];
        }
        out[i] = (uint8_t)(acc / 3);
    }
    return n;
}
