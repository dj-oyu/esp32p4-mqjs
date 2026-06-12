/* Host test for the barcode localizer + angled sampler, end to end:
 * a REAL EAN-13 rendered at an arbitrary rotation must be located
 * (bbox + theta), sampled along theta and decoded.
 *
 *   gcc -O2 -I../include -o /tmp/bc_test bc_locate_test.c \
 *       ../bc_locate.c ../ean13.c -lm
 *   /tmp/bc_test
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/bc_locate.h"
#include "../include/ean13.h"

#define FW 768
#define FH 576

static int fails;

static void expect(int cond, const char *name)
{
    if (cond)
        printf("ok %s\n", name);
    else {
        fails++;
        printf("FAIL %s\n", name);
    }
}

/* ---- minimal EAN-13 module renderer (same tables as ean13_test) ---- */
static const char *const L_BITS[10] = {
    "0001101", "0011001", "0010011", "0111101", "0100011",
    "0110001", "0101111", "0111011", "0110111", "0001011"
};
static const char *const PARITY[10] = {
    "LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
    "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"
};

static void with_checksum(const char *d12, char out[14])
{
    int sum = 0;
    for (int k = 0; k < 12; k++)
        sum += (d12[k] - '0') * ((k & 1) ? 3 : 1);
    memcpy(out, d12, 12);
    out[12] = (char)('0' + (10 - sum % 10) % 10);
    out[13] = '\0';
}

static void modules(const char code[14], char bits[96])
{
    char *p = bits;
    const char *par = PARITY[code[0] - '0'];
    memcpy(p, "101", 3);
    p += 3;
    for (int d = 0; d < 6; d++) {
        const char *l = L_BITS[code[1 + d] - '0'];
        if (par[d] == 'L') {
            memcpy(p, l, 7);
        } else {
            for (int k = 0; k < 7; k++)
                p[k] = l[6 - k] == '0' ? '1' : '0';
        }
        p += 7;
    }
    memcpy(p, "01010", 5);
    p += 5;
    for (int d = 0; d < 6; d++) {
        const char *l = L_BITS[code[7 + d] - '0'];
        for (int k = 0; k < 7; k++)
            p[k] = l[k] == '0' ? '1' : '0';
        p += 7;
    }
    memcpy(p, "101", 3);
    p[3] = '\0';
}

/* ---- synthetic frame ---- */
static uint16_t gray565(int l)
{
    if (l < 0)
        l = 0;
    if (l > 255)
        l = 255;
    return (uint16_t)(((l >> 3) << 11) | ((l >> 2) << 5) | (l >> 3));
}

static void background(uint16_t *f, unsigned seed)
{
    unsigned rnd = seed;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) {
            rnd = rnd * 1103515245u + 12345u;
            int l = 90 + x * 60 / FW + y * 30 / FH +
                    (int)((rnd >> 16) % 9) - 4;
            f[y * FW + x] = gray565(l);
        }
}

/* rotated EAN patch: bars perpendicular to direction `deg`, module
 * width `scale` px, bar half-height bh px, quiet zone 10 modules */
static void embed_rotated(uint16_t *f, const char code[14], int cx, int cy,
                          int deg, int scale, int bh, int dark, int light)
{
    char bits[96];
    modules(code, bits);
    float th = deg * (float)M_PI / 180.0f;
    float ux = cosf(th), uy = sinf(th);
    int quiet = 10;
    int half_u = (95 + 2 * quiet) * scale / 2;
    int reach = half_u + bh + 2;
    for (int y = cy - reach; y <= cy + reach; y++) {
        if (y < 0 || y >= FH)
            continue;
        for (int x = cx - reach; x <= cx + reach; x++) {
            if (x < 0 || x >= FW)
                continue;
            float pu = (x - cx) * ux + (y - cy) * uy;
            float pv = -(x - cx) * uy + (y - cy) * ux;
            if (pu < -half_u || pu >= half_u || pv < -bh || pv > bh)
                continue;
            int m = (int)((pu + half_u) / scale) - quiet;
            int v = (m >= 0 && m < 95 && bits[m] == '1') ? dark : light;
            f[y * FW + x] = gray565(v);
        }
    }
}

static int th_dist(int a, int b)
{
    int d = a - b;
    while (d > 90)
        d -= 180;
    while (d < -90)
        d += 180;
    return d < 0 ? -d : d;
}

/* 2x2 box downscale — on the device the PPA produces this in hardware;
 * the localizer must see an anti-aliased image (sharp full-res bars
 * alias into bogus diagonal orientations) */
static void halve(const uint16_t *f, uint16_t *half, int w, int h)
{
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            const uint16_t *p = f + (y * 2) * w + x * 2;
            int l = 0;
            for (int k = 0; k < 2; k++)
                for (int j = 0; j < 2; j++) {
                    uint16_t v = p[k * w + j];
                    l += ((((v >> 11) & 31) << 3) * 77 +
                          (((v >> 5) & 63) << 2) * 150 +
                          ((v & 31) << 3) * 29) >> 8;
                }
            half[y * (w / 2) + x] = gray565(l / 4);
        }
}

/* locate (on the halved image), then sample 9 offset lines along theta
 * on the FULL-RES frame and try to decode */
static int locate_and_decode(const uint16_t *f, char out[14],
                             bc_region_t *r)
{
    static uint8_t line[2048];
    static uint16_t ana[(FW / 2) * (FH / 2)];
    halve(f, ana, FW, FH);
    if (!bc_locate(ana, FW / 2, FH / 2, 2, r))
        return 0;
    int dx = r->x1 - r->x0, dy = r->y1 - r->y0;
    int half = (int)(sqrtf((float)(dx * dx + dy * dy)) / 2) + 48;
    for (int k = -4; k <= 4; k++) {
        int n = bc_sample_line(f, FW, FH, r->cx, r->cy, r->theta, k * 12,
                               half, line, (int)sizeof line);
        ean13_scan_t st;
        if (ean13_scan_gray_line(line, n, &st)) {
            memcpy(out, st.code, 14);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    static uint16_t frame[FW * FH];
    bc_region_t r;
    char code[14], got[14], name[80];

    with_checksum("978410101001", code);

    static const int angles[] = { 0, 30, 45, 60, 90, 120, 150 };
    for (size_t a = 0; a < sizeof angles / sizeof angles[0]; a++) {
        background(frame, 10 + (unsigned)a);
        embed_rotated(frame, code, FW / 2, FH / 2, angles[a], 3, 64, 40,
                      210);
        int hit = locate_and_decode(frame, got, &r);
        snprintf(name, sizeof name, "rotated %d deg: locate+decode",
                 angles[a]);
        expect(hit && strcmp(got, code) == 0, name);
        snprintf(name, sizeof name, "rotated %d deg: theta=%d", angles[a],
                 r.theta);
        expect(r.found && th_dist(r.theta, angles[a]) <= 12, name);
    }

    /* weak contrast at a slant (the user's shadow case) */
    background(frame, 99);
    embed_rotated(frame, code, FW / 2, FH / 2, 30, 4, 64, 110, 150);
    expect(locate_and_decode(frame, got, &r) && strcmp(got, code) == 0,
           "weak contrast at 30 deg");

    /* no barcode: gradient + noise only must NOT trigger */
    background(frame, 4);
    {
        static uint16_t half[(FW / 2) * (FH / 2)];
        halve(frame, half, FW, FH);
        expect(!bc_locate(half, FW / 2, FH / 2, 2, &r),
               "plain background rejected");
    }

    if (fails) {
        printf("bc_locate selftest: %d FAILED\n", fails);
        return 1;
    }
    printf("bc_locate selftest: ALL PASS\n");
    return 0;
}
