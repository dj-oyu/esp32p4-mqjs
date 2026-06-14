#include "qr_marker.h"

#include <stdlib.h>
#include <string.h>

#define MAX_CANDIDATES 48

typedef struct {
    int x, y, hits;
} marker_t;

static uint8_t otsu(const uint8_t *gray, int w, int h)
{
    unsigned hist[256] = { 0 };
    unsigned total = 0;
    unsigned long sum = 0, sum_b = 0;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            uint8_t v = gray[y * w + x];
            hist[v]++;
            sum += v;
            total++;
        }
    unsigned bg = 0;
    unsigned long best = 0;
    uint8_t threshold = 128;
    for (int i = 0; i < 256; i++) {
        bg += hist[i];
        if (!bg)
            continue;
        unsigned fg = total - bg;
        if (!fg)
            break;
        sum_b += (unsigned long)i * hist[i];
        long d = (long)(sum_b * total) - (long)(sum * bg);
        unsigned long score = (unsigned long)(d < 0 ? -d : d);
        score = score / bg * score / fg;
        if (score >= best) {
            best = score;
            threshold = (uint8_t)i;
        }
    }
    return threshold < 255 ? (uint8_t)(threshold + 1) : threshold;
}

static bool ratio_ok(const int r[5])
{
    int total = r[0] + r[1] + r[2] + r[3] + r[4];
    if (total < 14)
        return false;
    int unit = total / 7;
    int tol = unit;
    return abs(r[0] - unit) <= tol &&
           abs(r[1] - unit) <= tol &&
           abs(r[2] - 3 * unit) <= 3 * tol &&
           abs(r[3] - unit) <= tol &&
           abs(r[4] - unit) <= tol;
}

static bool vertical_ok(const uint8_t *gray, int w, int h, int x, int y,
                        uint8_t threshold)
{
    int run[5] = { 0 };
    int center = gray[y * w + x] >= threshold;
    int top = y, bottom = y;
    while (top >= 0 && ((gray[top * w + x] >= threshold) == center)) {
        run[2]++;
        top--;
    }
    while (++bottom < h && ((gray[bottom * w + x] >= threshold) == center))
        run[2]++;
    int yy = top;
    for (int k = 1; k >= 0; k--) {
        int wanted = ((2 - k) & 1) ? !center : center;
        while (yy >= 0 && ((gray[yy * w + x] >= threshold) == wanted)) {
            run[k]++;
            yy--;
        }
    }
    yy = bottom;
    for (int k = 3; k <= 4; k++) {
        int wanted = ((k - 2) & 1) ? !center : center;
        while (yy < h && ((gray[yy * w + x] >= threshold) == wanted)) {
            run[k]++;
            yy++;
        }
    }
    return ratio_ok(run);
}

static void add_marker(marker_t *m, int *n, int x, int y, int radius)
{
    for (int i = 0; i < *n; i++) {
        int dx = m[i].x - x, dy = m[i].y - y;
        if (dx * dx + dy * dy <= radius * radius) {
            m[i].x = (m[i].x * m[i].hits + x) / (m[i].hits + 1);
            m[i].y = (m[i].y * m[i].hits + y) / (m[i].hits + 1);
            m[i].hits++;
            return;
        }
    }
    if (*n < MAX_CANDIDATES)
        m[(*n)++] = (marker_t){ .x = x, .y = y, .hits = 1 };
}

static int find_markers(const uint8_t *gray, int w, int h, uint8_t threshold,
                        bool inverted, marker_t *markers)
{
    int n = 0;
    for (int y = 0; y < h; y += 2) {
        int run[5] = { 0 }, colors[5] = { 0 }, count = 0;
        int last = (gray[y * w] >= threshold);
        for (int x = 0; x <= w; x++) {
            int color = x < w ? (gray[y * w + x] >= threshold) : !last;
            if (color == last) {
                if (count < 5)
                    run[count]++;
                continue;
            }
            if (count < 5) {
                colors[count] = last;
                count++;
            }
            if (count == 5 && colors[2] == inverted && ratio_ok(run)) {
                int center = x - run[4] - run[3] - run[2] / 2;
                int radius = (run[0] + run[1] + run[2] + run[3] + run[4]) / 7 * 3;
                if (center >= 0 && center < w &&
                    vertical_ok(gray, w, h, center, y, threshold))
                    add_marker(markers, &n, center, y, radius > 3 ? radius : 3);
            }
            if (count == 5) {
                memmove(run, run + 1, 4 * sizeof(run[0]));
                memmove(colors, colors + 1, 4 * sizeof(colors[0]));
                run[4] = 0;
                colors[4] = color;
                count = 4;
            }
            last = color;
            run[count] = 1;
        }
    }
    return n;
}

static bool has_triangle(const marker_t *m, int n)
{
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            for (int c = b + 1; c < n; c++) {
                if (m[a].hits < 2 || m[b].hits < 2 || m[c].hits < 2)
                    continue;
                long dx = m[b].x - m[a].x, dy = m[b].y - m[a].y;
                long d[3] = { dx * dx + dy * dy, 0, 0 };
                dx = m[c].x - m[a].x; dy = m[c].y - m[a].y;
                d[1] = dx * dx + dy * dy;
                dx = m[c].x - m[b].x; dy = m[c].y - m[b].y;
                d[2] = dx * dx + dy * dy;
                for (int i = 0; i < 2; i++)
                    for (int j = i + 1; j < 3; j++)
                        if (d[i] > d[j]) {
                            long t = d[i]; d[i] = d[j]; d[j] = t;
                        }
                /* QR finder centers form an isosceles right triangle. */
                if (d[0] > 400 && d[0] * 2 > d[1] &&
                    labs(d[2] - d[0] - d[1]) < d[2] / 3)
                    return true;
            }
    return false;
}

bool qr_marker_detect(const uint8_t *gray, int w, int h,
                      qr_marker_result_t *result)
{
    uint8_t threshold = otsu(gray, w, h);
    marker_t normal[MAX_CANDIDATES], inverted[MAX_CANDIDATES];
    int nn = find_markers(gray, w, h, threshold, false, normal);
    if (has_triangle(normal, nn)) {
        *result = (qr_marker_result_t){ .markers = nn, .inverted = false };
        return true;
    }
    int ni = find_markers(gray, w, h, threshold, true, inverted);
    *result = (qr_marker_result_t){ .markers = ni, .inverted = true };
    return has_triangle(inverted, ni);
}
