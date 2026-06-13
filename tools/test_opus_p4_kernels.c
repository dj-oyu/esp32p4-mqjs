#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "opus_p4_kernels.h"

static void check(const float *a, const float *b, int n)
{
    float reference = opus_p4_inner_prod_f32_c(a, b, n);
    float dispatched = opus_p4_inner_prod_f32(a, b, n);
    assert(fabsf(reference - dispatched) <= 1e-6f);
}

int main(void)
{
    static const float a[] = { 1.0f, -2.0f, 3.5f, 0.25f, -9.0f };
    static const float b[] = { 2.0f, 4.0f, -1.0f, 8.0f, -0.5f };

    check(a, b, 0);
    check(a, b, 1);
    check(a, b, 5);
    check(a + 1, b + 1, 4);

    printf("opus_p4_kernels: %s OK\n", opus_p4_kernel_impl());
    return 0;
}
