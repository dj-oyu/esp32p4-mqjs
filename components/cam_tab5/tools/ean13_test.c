/* Host unit test for the EAN-13 scanline decoder (no IDF needed):
 *
 *   gcc -O2 -I../include -o /tmp/ean13_test ean13_test.c ../ean13.c
 *   /tmp/ean13_test
 *
 * Renders synthetic barcodes (module scale 2..6 px, ±noise, mild box
 * blur, both directions, off-center placement) and checks decode;
 * plus negative cases (noise, flat, corrupted digit).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/ean13.h"

static const char *const L_BITS[10] = {
    "0001101", "0011001", "0010011", "0111101", "0100011",
    "0110001", "0101111", "0111011", "0110111", "0001011"
};
static const char *const PARITY[10] = {
    "LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
    "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"
};

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

/* 12 digits in, full 13-digit code with checksum out */
static void with_checksum(const char *d12, char out[14])
{
    int sum = 0;
    for (int k = 0; k < 12; k++)
        sum += (d12[k] - '0') * ((k & 1) ? 3 : 1);
    memcpy(out, d12, 12);
    out[12] = (char)('0' + (10 - sum % 10) % 10);
    out[13] = '\0';
}

/* 95 module bits for a 13-digit code */
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
        } else { /* G = reverse of complement-of-L */
            for (int k = 0; k < 7; k++)
                p[k] = l[6 - k] == '0' ? '1' : '0';
        }
        p += 7;
    }
    memcpy(p, "01010", 5);
    p += 5;
    for (int d = 0; d < 6; d++) {
        const char *l = L_BITS[code[7 + d] - '0'];
        for (int k = 0; k < 7; k++) /* R = complement of L */
            p[k] = l[k] == '0' ? '1' : '0';
        p += 7;
    }
    memcpy(p, "101", 3);
    p[3] = '\0';
}

/* render to a gray line: quiet zones, scale px/module, noise, blur */
static int render(const char code[14], uint8_t *line, int cap, int scale,
                  int noise, int reversed, unsigned seed)
{
    char bits[96];
    modules(code, bits);
    int quiet = 12 * scale;
    int n = quiet * 2 + 95 * scale;
    if (n > cap)
        return 0;
    unsigned rnd = seed;
    for (int i = 0; i < n; i++) {
        int m = (i - quiet) / scale;
        int dark = (i >= quiet && m < 95 && bits[m] == '1');
        int v = dark ? 40 : 210;
        rnd = rnd * 1103515245u + 12345u;
        v += (int)((rnd >> 16) % (2 * noise + 1)) - noise;
        if (v < 0)
            v = 0;
        if (v > 255)
            v = 255;
        line[i] = (uint8_t)v;
    }
    if (reversed) {
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            uint8_t t = line[i];
            line[i] = line[j];
            line[j] = t;
        }
    }
    /* mild optical blur: 3-tap box */
    for (int pass = 0; pass < 1; pass++) {
        uint8_t prev = line[0];
        for (int i = 1; i < n - 1; i++) {
            uint8_t cur = line[i];
            line[i] = (uint8_t)((prev + cur + line[i + 1]) / 3);
            prev = cur;
        }
    }
    return n;
}

int main(void)
{
    static uint8_t line[4096];
    char code[14], got[14], name[96];

    const char *isbn12[] = {
        "978410101001", /* 吾輩は猫である */
        "978416711011", /* 手紙 */
        "977477414204", /* 979/978 variants */
        "979123456789",
        "192027601800", /* 書籍JAN 2 段目 (decoder reads it; cam filters) */
        "490123456789"  /* generic JAN */
    };

    for (size_t c = 0; c < sizeof isbn12 / sizeof isbn12[0]; c++) {
        with_checksum(isbn12[c], code);
        for (int scale = 2; scale <= 6; scale++) {
            for (int rev = 0; rev <= 1; rev++) {
                int n = render(code, line, sizeof line, scale, 12, rev,
                               (unsigned)(c * 31 + scale * 7 + rev));
                int ok = n && ean13_decode_gray_line(line, n, got) &&
                         strcmp(got, code) == 0;
                snprintf(name, sizeof name, "%s scale=%d rev=%d", code,
                         scale, rev);
                expect(ok, name);
            }
        }
    }

    /* high noise still decodes at a healthy scale */
    with_checksum(isbn12[0], code);
    int n = render(code, line, sizeof line, 5, 35, 0, 99);
    expect(n && ean13_decode_gray_line(line, n, got) &&
               strcmp(got, code) == 0,
           "noisy scale=5");

    /* real-photo conditions: the barcode embedded mid-line with cover
       art (dark/bright blocks) outside the quiet zones plus a strong
       illumination ramp — a global threshold fails this, the local
       adaptive one must not */
    for (int scale = 3; scale <= 5; scale++) {
        with_checksum(isbn12[1], code);
        static uint8_t big[4096];
        int bn = 1600;
        unsigned rnd2 = 1234u + (unsigned)scale;
        for (int i = 0; i < bn; i++) { /* cover clutter */
            rnd2 = rnd2 * 1103515245u + 12345u;
            big[i] = (uint8_t)(((rnd2 >> 16) & 1) ? 30 : 235);
        }
        n = render(code, line, sizeof line, scale, 10, 0, 7);
        int off = (bn - n) / 2;
        memcpy(big + off, line, (size_t)n);
        for (int i = 0; i < bn; i++) { /* +60 ramp across the line */
            int v = big[i] + i * 60 / bn;
            big[i] = (uint8_t)(v > 255 ? 255 : v);
        }
        snprintf(name, sizeof name, "clutter+ramp scale=%d", scale);
        expect(ean13_decode_gray_line(big, bn, got) &&
                   strcmp(got, code) == 0,
               name);
    }

    /* negatives: random noise lines must never decode */
    unsigned rnd = 7;
    int false_pos = 0;
    for (int t = 0; t < 2000; t++) {
        for (int i = 0; i < 1280; i++) {
            rnd = rnd * 1103515245u + 12345u;
            line[i] = (uint8_t)(rnd >> 16);
        }
        if (ean13_decode_gray_line(line, 1280, got))
            false_pos++;
    }
    expect(false_pos == 0, "no false positives on 2000 noise lines");

    /* flat line rejected */
    memset(line, 128, 1280);
    expect(!ean13_decode_gray_line(line, 1280, got), "flat line rejected");

    /* corrupted check digit rejected */
    with_checksum(isbn12[1], code);
    code[12] = code[12] == '9' ? '0' : (char)(code[12] + 1);
    n = render(code, line, sizeof line, 4, 5, 0, 3);
    expect(n && !ean13_decode_gray_line(line, n, got),
           "bad checksum rejected");

    /* weak contrast (barcode in shadow / AWB washout): squeeze the
       rendered 40..210 range into 110..150 — must still decode */
    for (int scale = 3; scale <= 5; scale++) {
        with_checksum(isbn12[0], code);
        n = render(code, line, sizeof line, scale, 4, 0, 11);
        for (int i = 0; i < n; i++)
            line[i] = (uint8_t)(110 + (line[i] - 40) * 40 / 170);
        snprintf(name, sizeof name, "weak contrast scale=%d", scale);
        expect(ean13_decode_gray_line(line, n, got) &&
                   strcmp(got, code) == 0,
               name);
    }

    /* extended telemetry API: found -> digits 13 + sane span */
    {
        ean13_scan_t st;
        with_checksum(isbn12[0], code);
        n = render(code, line, sizeof line, 4, 8, 0, 5);
        int quiet = 12 * 4, span = 95 * 4;
        expect(ean13_scan_gray_line(line, n, &st) && st.found &&
                   st.digits == 13 && strcmp(st.code, code) == 0,
               "scan_ex found digits=13");
        expect(st.x0 > quiet - 12 && st.x0 < quiet + 12 &&
                   st.x1 > quiet + span - 12 && st.x1 < quiet + span + 12,
               "scan_ex span position");

        /* corrupted checksum -> near-miss with digits == 12 and span */
        with_checksum(isbn12[0], code);
        code[12] = code[12] == '9' ? '0' : (char)(code[12] + 1);
        n = render(code, line, sizeof line, 4, 5, 0, 9);
        expect(!ean13_scan_gray_line(line, n, &st) && !st.found &&
                   st.digits == 12 && st.x1 > st.x0,
               "scan_ex near-miss digits=12");

        /* reversed direction still maps the span to original coords */
        with_checksum(isbn12[0], code);
        n = render(code, line, sizeof line, 4, 8, 1, 5);
        expect(ean13_scan_gray_line(line, n, &st) && st.found &&
                   st.x0 > quiet - 12 && st.x1 < quiet + span + 12 &&
                   st.x1 > st.x0,
               "scan_ex reversed span");
    }

    if (fails) {
        printf("ean13 selftest: %d FAILED\n", fails);
        return 1;
    }
    printf("ean13 selftest: ALL PASS\n");
    return 0;
}
