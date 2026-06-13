#include "opus_p4_kernels.h"

#if defined(CONFIG_OPUS_P4_KERNEL_ASM) || defined(CONFIG_OPUS_P4_KERNEL_ASM_VERIFY)
#error "P4 PIE Opus kernels are selected but have not been implemented yet"
#endif

typedef float (*inner_prod_f32_fn)(const float *, const float *, int);

typedef struct {
    inner_prod_f32_fn inner_prod_f32;
    const char *name;
} opus_kernel_ops_t;

float opus_p4_inner_prod_f32_c(const float *a, const float *b, int n)
{
    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += a[i] * b[i];
    return sum;
}

/*
 * The dispatch object exists from the first integration step so codec call
 * sites do not change when PIE kernels replace individual C functions.
 */
static const opus_kernel_ops_t s_ops = {
    .inner_prod_f32 = opus_p4_inner_prod_f32_c,
    .name = "portable-c",
};

float opus_p4_inner_prod_f32(const float *a, const float *b, int n)
{
    return s_ops.inner_prod_f32(a, b, n);
}

const char *opus_p4_kernel_impl(void)
{
    return s_ops.name;
}
