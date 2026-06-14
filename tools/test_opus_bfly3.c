/*
 * Host bit-exact test for opus_p4_bfly3_c.
 *
 * Validates the kernel C reference against an independent golden that runs the
 * original kf_bfly3 macro form (with a 64-bit MULT16_32_Q15 and modular
 * 32-bit add/sub) over random complex data, twiddles, m, fstride, and the
 * epi3_i scalar, including overflow-wrap boundaries.
 *
 * Build/run (host, WSL):
 *   gcc -O2 -Wall -I components/opus_p4_kernels/include \
 *       tools/test_opus_bfly3.c components/opus_p4_kernels/opus_p4_kernels.c \
 *       -o /tmp/test_opus_bfly3 && /tmp/test_opus_bfly3
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "opus_p4_kernels.h"

/* MULT16_32_Q15, FAST_INT64=1 form: full 64-bit product, arithmetic >>15,
 * truncated to int32 -- the exact value the FAST_INT64=0 decomposition in the
 * kernel reproduces. */
static int32_t gM(int16_t t, int32_t d)
{
    return (int32_t)(((int64_t)t * (int64_t)d) >> 15);
}
static int32_t gADD(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a + (uint32_t)b);
}
static int32_t gSUB(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

/* Independent golden: same macro structure as celt/kiss_fft.c kf_bfly3, with
 * S_MUL(d,t) = MULT16_32_Q15(t,d). */
static void golden_bfly3(int32_t *Fout, int m, const int16_t *tw, int fstride,
                         int16_t epi3_i)
{
    int m2 = 2 * m;
    for (int k = 0; k < m; k++) {
        int32_t *Fa = Fout + 2 * k;
        int32_t *Fb = Fout + 2 * (m + k);
        int32_t *Fc = Fout + 2 * (m2 + k);
        const int16_t *tw1 = tw + 2 * (k * fstride);
        const int16_t *tw2 = tw + 2 * (k * 2 * fstride);
        int32_t s1r = gSUB(gM(tw1[0], Fb[0]), gM(tw1[1], Fb[1]));
        int32_t s1i = gADD(gM(tw1[0], Fb[1]), gM(tw1[1], Fb[0]));
        int32_t s2r = gSUB(gM(tw2[0], Fc[0]), gM(tw2[1], Fc[1]));
        int32_t s2i = gADD(gM(tw2[0], Fc[1]), gM(tw2[1], Fc[0]));
        int32_t s3r = gADD(s1r, s2r), s3i = gADD(s1i, s2i);
        int32_t s0r = gSUB(s1r, s2r), s0i = gSUB(s1i, s2i);
        int32_t fbr = gSUB(Fa[0], s3r >> 1);
        int32_t fbi = gSUB(Fa[1], s3i >> 1);
        s0r = gM(epi3_i, s0r);
        s0i = gM(epi3_i, s0i);
        Fa[0] = gADD(Fa[0], s3r);
        Fa[1] = gADD(Fa[1], s3i);
        Fc[0] = gADD(fbr, s0i);
        Fc[1] = gSUB(fbi, s0r);
        Fb[0] = gSUB(fbr, s0i);
        Fb[1] = gADD(fbi, s0r);
    }
}

static uint32_t rng = 0xBEEF01u;
static uint32_t xr(void)
{
    rng = 1664525u * rng + 1013904223u;
    return rng;
}

#define MAXM 64
#define TWLEN (MAXM * 4 * 2 + 16) /* up to (m-1)*2*fstride complex, *2 for r,i */

static int run_case(int m, int fstride, int16_t epi3_i, int bigdata)
{
    int32_t Fa[3 * MAXM * 2], Fb[3 * MAXM * 2];
    int16_t tw[TWLEN];
    for (int j = 0; j < 3 * m * 2; j++) {
        uint32_t v = xr();
        /* Either ~Q-range samples or near-full-int32 to hit overflow wrap. */
        Fa[j] = bigdata ? (int32_t)v : ((int32_t)v >> 6);
        Fb[j] = Fa[j];
    }
    for (int j = 0; j < TWLEN; j++)
        tw[j] = (int16_t)xr();

    golden_bfly3(Fa, m, tw, fstride, epi3_i);
    opus_p4_bfly3_c(Fb, m, tw, fstride, epi3_i);

    if (memcmp(Fa, Fb, 3 * m * 2 * sizeof(int32_t)) != 0) {
        for (int j = 0; j < 3 * m * 2; j++)
            if (Fa[j] != Fb[j]) {
                printf("MISMATCH m=%d fstride=%d epi3=%d j=%d golden=%ld kernel=%ld\n",
                       m, fstride, epi3_i, j, (long)Fa[j], (long)Fb[j]);
                return 1;
            }
    }
    return 0;
}

int main(void)
{
    int fails = 0, cases = 0;
    static const int ms[] = { 1, 2, 4, 8, 16, 32, 64 };
    static const int fs[] = { 1, 2, 3, 4 };
    static const int16_t epis[] = { -28377, 0, 1, -1, 32767, -32768, -16384 };

    for (size_t mi = 0; mi < sizeof(ms) / sizeof(ms[0]); mi++)
        for (size_t fi = 0; fi < sizeof(fs) / sizeof(fs[0]); fi++)
            for (size_t ei = 0; ei < sizeof(epis) / sizeof(epis[0]); ei++)
                for (int big = 0; big < 2; big++) {
                    /* keep (m-1)*2*fstride within TWLEN */
                    if ((ms[mi] - 1) * 2 * fs[fi] * 2 + 2 >= TWLEN)
                        continue;
                    fails += run_case(ms[mi], fs[fi], epis[ei], big);
                    cases++;
                }

    for (int t = 0; t < 20000; t++) {
        int m = 1 + (xr() % MAXM);
        int fstride = 1 + (xr() % 4);
        if ((m - 1) * 2 * fstride * 2 + 2 >= TWLEN)
            continue;
        int16_t epi3 = (int16_t)xr();
        fails += run_case(m, fstride, epi3, xr() & 1);
        cases++;
    }

    printf("%s: %d cases, %d failures\n", fails ? "FAIL" : "PASS", cases, fails);
    return fails ? 1 : 0;
}
