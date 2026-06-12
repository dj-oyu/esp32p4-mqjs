/* Barcode-region localization + angled scanline sampling.
 * See include/bc_locate.h. */
#include "bc_locate.h"

#include <math.h>
#include <string.h>

#define BLK 32        /* block size in ANALYSIS-image px (x2 in frame) */
#define GRID BLK      /* every pixel of the downscaled image */
#define MAX_BX 32     /* up to 1024px-wide analysis images */
#define MAX_BY 24

/* energy floor: a barcode block (even in shadow, ~40 gray levels of
 * contrast) accumulates squared edge gradients in the 1e5 range;
 * sensor noise and smooth lighting stay an order of magnitude lower */
#define E_MIN 60000
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

/* tensor-only luma: the green channel alone (59% of luminance, and
 * barcodes are achromatic anyway) — one shift+mask instead of three
 * multiplies, in the hottest per-pixel loop we own */
static inline int luma_fast(uint16_t v)
{
    return (v >> 3) & 0xFC;
}

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
               biases Sxy toward +45° and mirrors angles beyond 90° */
            int l[GRID][GRID];
            for (int sy = 0; sy < GRID; sy++) {
                const uint16_t *row = base + sy * w;
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
