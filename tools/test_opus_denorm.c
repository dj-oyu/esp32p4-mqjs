/*
 * Host bit-exact test for opus_p4_denorm_band_c.
 *
 * Validates the kernel's portable C reference (RV32 MULT16_32_Q15
 * decomposition + arithmetic SHR32) against an independent int64 golden model
 * of the denormalise_bands common path, over random x/g/shift and boundary
 * inputs.
 *
 * Build/run (host, WSL):
 *   gcc -O2 -Wall -I components/opus_p4_kernels/include \
 *       tools/test_opus_denorm.c components/opus_p4_kernels/opus_p4_kernels.c \
 *       -o /tmp/test_opus_denorm && /tmp/test_opus_denorm
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "opus_p4_kernels.h"

/* Independent golden: full-precision 64-bit product, arithmetic >>15,
 * truncated to int32 (FAST_INT64=1 form of MULT16_32_Q15), then arithmetic
 * SHR32 by shift. */
static int32_t golden_m(int16_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 15);
}

static void golden_denorm(int32_t *f, const int16_t *x, int N, int32_t g,
                          int shift)
{
    for (int j = 0; j < N; j++)
        f[j] = golden_m(x[j], g) >> shift;
}

static uint32_t rng = 0x12345678u;
static uint32_t xr(void)
{
    rng = 1664525u * rng + 1013904223u;
    return rng;
}

#define MAXN 520

static int run_case(int N, int32_t g, int shift)
{
    int16_t x[MAXN];
    int32_t fa[MAXN], fb[MAXN];
    for (int j = 0; j < N; j++)
        x[j] = (int16_t)xr();

    golden_denorm(fa, x, N, g, shift);
    opus_p4_denorm_band_c(fb, x, N, g, shift);

    for (int j = 0; j < N; j++) {
        if (fa[j] != fb[j]) {
            printf("MISMATCH N=%d g=%ld shift=%d j=%d x=%d golden=%ld kernel=%ld\n",
                   N, (long)g, shift, j, x[j], (long)fa[j], (long)fb[j]);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    int fails = 0;
    int cases = 0;

    /* Boundary g values: 0, small, near 2^31, sign-bit low half, powers. */
    static const int32_t gs[] = {
        0, 1, -1, 32768, -32768, 0x00008000, 0x0000ffff, 0x7fffffff,
        (int32_t)0x80000000, 0x12340000, 0x0000abcd, 16384 * 32768,
        0x55555555, (int32_t)0xaaaaaaaa, 100, -100,
    };
    static const int shifts[] = { 0, 1, 2, 7, 12, 15, 20, 31 };
    static const int Ns[] = { 1, 2, 3, 7, 8, 9, 15, 16, 17, 30, 64, 511, 512 };

    for (size_t gi = 0; gi < sizeof(gs) / sizeof(gs[0]); gi++)
        for (size_t si = 0; si < sizeof(shifts) / sizeof(shifts[0]); si++)
            for (size_t ni = 0; ni < sizeof(Ns) / sizeof(Ns[0]); ni++) {
                fails += run_case(Ns[ni], gs[gi], shifts[si]);
                cases++;
            }

    /* Random fuzz. */
    for (int t = 0; t < 20000; t++) {
        int N = 1 + (xr() % MAXN);
        int32_t g = (int32_t)xr();
        int shift = xr() % 32;
        fails += run_case(N, g, shift);
        cases++;
    }

    printf("%s: %d cases, %d failures\n", fails ? "FAIL" : "PASS", cases, fails);
    return fails ? 1 : 0;
}
