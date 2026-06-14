/*
 * Host bit-exact test for opus_p4_comb_filter_const_c.
 *
 * Validates the kernel's portable C reference (which uses the RV32
 * MULT16_32_Q15 decomposition) against an independent int64 golden model of
 * comb_filter_const_c, over random data, gain, and saturation-boundary inputs.
 *
 * Build/run (host):
 *   gcc -O2 -Wall -I components/opus_p4_kernels/include \
 *       tools/test_opus_comb.c components/opus_p4_kernels/opus_p4_kernels.c \
 *       -o /tmp/test_opus_comb && /tmp/test_opus_comb
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "opus_p4_kernels.h"

#define SIG_SAT 536870911 /* 2^29 - 1 */

/* Independent golden: full-precision 64-bit product, arithmetic >>15,
 * truncated to int32 -- the FAST_INT64=1 form of MULT16_32_Q15. */
static int32_t golden_m(int16_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 15);
}

static int32_t golden_sat(int64_t x)
{
    if (x > SIG_SAT)
        return SIG_SAT;
    if (x < -SIG_SAT)
        return -SIG_SAT;
    return (int32_t)x;
}

static void golden_comb(int32_t *y, const int32_t *x, int T, int N, int16_t g10,
                        int16_t g11, int16_t g12)
{
    for (int i = 0; i < N; i++) {
        /* 32-bit wraparound adds, matching ADD32/'+' in the codec. */
        uint32_t t = (uint32_t)x[i];
        t += (uint32_t)golden_m(g10, x[i - T]);
        t += (uint32_t)golden_m(
            g11, (int32_t)((uint32_t)x[i - T + 1] + (uint32_t)x[i - T - 1]));
        t += (uint32_t)golden_m(
            g12, (int32_t)((uint32_t)x[i - T + 2] + (uint32_t)x[i - T - 2]));
        y[i] = golden_sat((int32_t)t);
    }
}

static uint32_t rng = 0x12345678u;
static uint32_t urand(void)
{
    rng = 1664525u * rng + 1013904223u;
    return rng;
}
/* Bias towards large magnitudes so the saturation and wrap paths are hit. */
static int32_t rand_sig(void)
{
    uint32_t r = urand();
    switch (r & 3) {
    case 0:
        return (int32_t)r; /* full range */
    case 1:
        return (int32_t)(r % 0x40000000u) - 0x20000000; /* near +/-SIG_SAT */
    default:
        return (int32_t)(r % 0x2000u) - 0x1000; /* small */
    }
}

int main(void)
{
    enum { GUARD = 8, MAXN = 257, BUF = MAXN + 64 };
    static int32_t xbuf[BUF], yk[MAXN], yg[MAXN];
    long checks = 0;

    for (int trial = 0; trial < 200000; trial++) {
        int T = 2 + (int)(urand() % 40u);
        int N = 1 + (int)(urand() % (uint32_t)(MAXN - 1));
        int16_t g10 = (int16_t)urand();
        int16_t g11 = (int16_t)urand();
        int16_t g12 = (int16_t)urand();

        /* x must be readable from x[-T-2] to x[N-1-T+2]; place x at &xbuf[off]
         * with enough history. */
        int off = T + GUARD;
        for (int j = -off; j < N + GUARD; j++)
            xbuf[off + j] = rand_sig();
        const int32_t *x = &xbuf[off];

        opus_p4_comb_filter_const_c(yk, x, T, N, g10, g11, g12);
        golden_comb(yg, x, T, N, g10, g11, g12);
        for (int i = 0; i < N; i++) {
            if (yk[i] != yg[i]) {
                printf("MISMATCH trial=%d i=%d T=%d N=%d g=%d,%d,%d "
                       "kernel=%d golden=%d\n",
                       trial, i, T, N, g10, g11, g12, yk[i], yg[i]);
                return 1;
            }
            checks++;
        }
    }
    printf("opus_p4_comb_filter_const_c: %ld checks OK (bit-exact vs int64 "
           "golden)\n",
           checks);
    return 0;
}
