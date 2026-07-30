/*
 * Host bit-exact test for opus_p4_normres_band_c.
 *
 * Validates the kernel C reference against an independent int64 golden of the
 * CELT normalise_residual hot loop, over random iy/g/k and boundary inputs.
 *
 * Build/run (host, WSL):
 *   gcc -O2 -Wall -I components/opus_p4_kernels/include \
 *       tools/tests/test_opus_normres.c components/opus_p4_kernels/opus_p4_kernels.c \
 *       -o /tmp/test_opus_normres && /tmp/test_opus_normres
 */
#include <stdint.h>
#include <stdio.h>

#include "opus_p4_kernels.h"

/* Independent golden: 64-bit product of g and the int16-narrowed iy, plus the
 * PSHR32 round bias (1<<k), arithmetic >>(k+1), truncated to int16. */
static int16_t golden_one(int16_t g, int32_t iy, int k)
{
    int64_t p = (int64_t)g * (int64_t)(int16_t)iy;
    int64_t r = (p + ((int64_t)1 << k)) >> (k + 1);
    return (int16_t)r;
}

static uint32_t rng = 0xC0FFEEu;
static uint32_t xr(void)
{
    rng = 1664525u * rng + 1013904223u;
    return rng;
}

#define MAXN 520

static int run_case(int N, int16_t g, int k)
{
    int32_t iy[MAXN];
    int16_t Xa[MAXN], Xb[MAXN];
    for (int j = 0; j < N; j++) {
        /* VQ pulses: small signed ints, but exercise the full int16 range of
         * the (int16)iy cast too. */
        iy[j] = (int16_t)xr();
    }
    for (int j = 0; j < N; j++)
        Xa[j] = golden_one(g, iy[j], k);
    opus_p4_normres_band_c(Xb, iy, N, g, k);
    for (int j = 0; j < N; j++) {
        if (Xa[j] != Xb[j]) {
            printf("MISMATCH N=%d g=%d k=%d j=%d iy=%ld golden=%d kernel=%d\n",
                   N, g, k, j, (long)iy[j], Xa[j], Xb[j]);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    int fails = 0, cases = 0;
    static const int16_t gs[] = { 0, 1, -1, 32767, -32768, 16384, -16384,
                                  12345, -9999, 255, -256 };
    static const int ks[] = { 0, 1, 2, 3, 7, 10, 14, 15 };
    static const int Ns[] = { 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 64, 511, 512 };

    for (size_t gi = 0; gi < sizeof(gs) / sizeof(gs[0]); gi++)
        for (size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++)
            for (size_t ni = 0; ni < sizeof(Ns) / sizeof(Ns[0]); ni++) {
                fails += run_case(Ns[ni], gs[gi], ks[ki]);
                cases++;
            }

    for (int t = 0; t < 20000; t++) {
        int N = 1 + (xr() % MAXN);
        int16_t g = (int16_t)xr();
        int k = xr() % 16;
        fails += run_case(N, g, k);
        cases++;
    }

    printf("%s: %d cases, %d failures\n", fails ? "FAIL" : "PASS", cases, fails);
    return fails ? 1 : 0;
}
