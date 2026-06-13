#include "opus_p4_kernels.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The ESP headers are only needed by the verify and profiling blocks, both of
 * which are CONFIG_-gated off in a host build. Guard them so the portable C
 * kernels and references compile on the host for bit-exactness testing. */
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

typedef float (*inner_prod_f32_fn)(const float *, const float *, int);
typedef uint32_t (*anti_collapse_noise_f32_fn)(float *, int, int, float,
                                               uint32_t);

typedef struct {
    inner_prod_f32_fn inner_prod_f32;
    anti_collapse_noise_f32_fn anti_collapse_noise_f32;
    const char *name;
} opus_kernel_ops_t;

float opus_p4_inner_prod_f32_c(const float *a, const float *b, int n)
{
    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += a[i] * b[i];
    return sum;
}

uint32_t opus_p4_anti_collapse_noise_f32_c(float *x, int n, int stride, float r,
                                           uint32_t seed)
{
    for (int i = 0; i < n; i++) {
        seed = 1664525u * seed + 1013904223u;
        x[i * stride] = (seed & 0x8000u) ? r : -r;
    }
    return seed;
}

#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4
void opus_p4_pie_probe_run(const int16_t *in, const int32_t *in32, int32_t *out);
void opus_p4_comb_bench(void);

void opus_p4_pie_probe(void)
{
    static int16_t in[32] __attribute__((aligned(16)));
    static int32_t in32[16] __attribute__((aligned(16)));
    static int32_t out[40] __attribute__((aligned(16)));
    /* vmul widening with SIGNED operands: A={1,-2,3,-4,5,-6,7,-8},
     * B={-10,20,-30,40,-50,60,-70,80} -> -10,-40,-90,-160,-250,-360,-490,-640 */
    for (int i = 0; i < 8; i++) {
        int s = (i & 1) ? -1 : 1;
        in[i] = (int16_t)(s * (i + 1));
        in[8 + i] = (int16_t)(-s * (i + 1) * 10);
    }
    /* cmul: 4 complex data (r,i) x twiddle (0, 16384=+0.5 in Q15) */
    static const int16_t data[8] = { 100, 0, 0, 100, 100, 100, 200, 300 };
    static const int16_t tw[8] = { 0, 16384, 0, 16384, 0, 16384, 0, 16384 };
    for (int i = 0; i < 8; i++) {
        in[16 + i] = data[i];
        in[24 + i] = tw[i];
    }
    /* comb-inner trace: 8 const int32 = 0x100000 (lo=0, hi=16), g=9000 */
    for (int i = 0; i < 8; i++)
        in32[i] = 0x100000;
    in32[8] = 9000; /* g (low 16 used by vldbc.16) */
    memset(out, 0, sizeof(out));
    opus_p4_pie_probe_run(in, in32, out);
    ESP_LOGI("pie_probe", "vmul.s32.s16xs16 SIGNED (expect -10,-40,-90,-160 / -250,-360,-490,-640)");
    ESP_LOGI("pie_probe", "  dst0= %ld %ld %ld %ld dst1= %ld %ld %ld %ld",
             (long)out[0], (long)out[1], (long)out[2], (long)out[3],
             (long)out[4], (long)out[5], (long)out[6], (long)out[7]);
    ESP_LOGI("pie_probe", "cmul.s16 q6= 0x%08lx 0x%08lx 0x%08lx 0x%08lx",
             (long)out[8], (long)out[9], (long)out[10], (long)out[11]);
    ESP_LOGI("pie_probe", "comb-inner const=0x100000 g=9000:");
    ESP_LOGI("pie_probe", "  lo(exp 0)= %ld %ld  hi(exp 16)= %ld %ld",
             (long)out[20], (long)out[21], (long)out[24], (long)out[25]);
    ESP_LOGI("pie_probe", "  phi=g*hi(exp 144000)= %ld %ld  phi<<1(exp 288000)= %ld %ld",
             (long)out[28], (long)out[29], (long)out[32], (long)out[33]);
    ESP_LOGI("pie_probe", "  in-place vadd.s32 (exp 432000)= %ld %ld",
             (long)out[36], (long)out[37]);
    opus_p4_comb_bench();
}
#else
void opus_p4_pie_probe(void) {}
#endif

/* CELT SIG_SAT (2^29-1) and the exact MULT16_32_Q15 used in the RV32
 * (OPUS_FAST_INT64=0) fixed-point build. Kept local so this component stays
 * independent of the opus headers while reproducing their bit pattern. */
#define OPUS_P4_SIG_SAT 536870911

static inline int32_t p4_mult16_32_q15(int16_t a, int32_t b)
{
    /* ADD32(SHL(MULT16_16(a, b>>16), 1), SHR(MULT16_16SU(a, b&0xffff), 15)) */
    int32_t hi = (int32_t)((uint32_t)((int32_t)a * (int32_t)(int16_t)(b >> 16))
                           << 1);
    int32_t lo = ((int32_t)a * (int32_t)(uint16_t)(b & 0xffffu)) >> 15;
    return (int32_t)((uint32_t)hi + (uint32_t)lo);
}

static inline int32_t p4_sig_sat(int32_t x)
{
    if (x > OPUS_P4_SIG_SAT)
        return OPUS_P4_SIG_SAT;
    if (x < -OPUS_P4_SIG_SAT)
        return -OPUS_P4_SIG_SAT;
    return x;
}

void opus_p4_comb_filter_const_c(int32_t *y, const int32_t *x, int T, int N,
                                 int16_t g10, int16_t g11, int16_t g12)
{
    for (int i = 0; i < N; i++) {
        int32_t s = x[i - T];
        int32_t a = (int32_t)((uint32_t)x[i - T + 1] + (uint32_t)x[i - T - 1]);
        int32_t b = (int32_t)((uint32_t)x[i - T + 2] + (uint32_t)x[i - T - 2]);
        int32_t t = x[i];
        t = (int32_t)((uint32_t)t + (uint32_t)p4_mult16_32_q15(g10, s));
        t = (int32_t)((uint32_t)t + (uint32_t)p4_mult16_32_q15(g11, a));
        t = (int32_t)((uint32_t)t + (uint32_t)p4_mult16_32_q15(g12, b));
        y[i] = p4_sig_sat(t);
    }
}

#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32P4
void opus_p4_comb8(int32_t *y, const int32_t *x, int T, int16_t g10,
                   int16_t g11, int16_t g12);

void opus_p4_comb_filter_const_p4(int32_t *y, const int32_t *x, int T, int N,
                                  int16_t g10, int16_t g11, int16_t g12)
{
    int i = 0;
    int n8 = N & ~7;
    for (; i < n8; i += 8)
        opus_p4_comb8(y + i, x + i, T, g10, g11, g12);
    for (; i < N; i++) {
        int32_t s = x[i - T];
        int32_t a = (int32_t)((uint32_t)x[i - T + 1] + (uint32_t)x[i - T - 1]);
        int32_t b = (int32_t)((uint32_t)x[i - T + 2] + (uint32_t)x[i - T - 2]);
        int32_t t = x[i];
        t = (int32_t)((uint32_t)t + (uint32_t)p4_mult16_32_q15(g10, s));
        t = (int32_t)((uint32_t)t + (uint32_t)p4_mult16_32_q15(g11, a));
        t = (int32_t)((uint32_t)t + (uint32_t)p4_mult16_32_q15(g12, b));
        y[i] = p4_sig_sat(t);
    }
}

void opus_p4_comb_bench(void)
{
    enum { N = 480, T = 100, GUARD = 16, BUF = N + T + GUARD + 16 };
    static int32_t xbuf[BUF];
    static int32_t yc[N], yp[N];
    uint32_t seed = 12345u;
    for (int i = 0; i < BUF; i++) {
        seed = 1664525u * seed + 1013904223u;
        xbuf[i] = (int32_t)seed >> 4; /* +/- ~2^27 */
    }
    const int32_t *x = &xbuf[T + GUARD];
    int16_t g10 = 9000, g11 = 6000, g12 = 3000;
    const int iters = 2000;

    uint32_t t0 = esp_cpu_get_cycle_count();
    for (int it = 0; it < iters; it++)
        opus_p4_comb_filter_const_c(yc, x, T, N, g10, g11, g12);
    uint32_t tc = esp_cpu_get_cycle_count() - t0;

    t0 = esp_cpu_get_cycle_count();
    for (int it = 0; it < iters; it++)
        opus_p4_comb_filter_const_p4(yp, x, T, N, g10, g11, g12);
    uint32_t tp = esp_cpu_get_cycle_count() - t0;

    int32_t maxdiff = 0;
    int imax = 0;
    for (int i = 0; i < N; i++) {
        int32_t d = yc[i] - yp[i];
        if (d < 0)
            d = -d;
        if (d > maxdiff) {
            maxdiff = d;
            imax = i;
        }
    }
    ESP_LOGI("comb_bench",
             "imax=%d yc=%ld yp=%ld | x[i]=%ld x[i-T]=%ld xm1=%ld xp1=%ld xm2=%ld xp2=%ld",
             imax, (long)yc[imax], (long)yp[imax], (long)x[imax],
             (long)x[imax - T], (long)x[imax - T - 1], (long)x[imax - T + 1],
             (long)x[imax - T - 2], (long)x[imax - T + 2]);
    ESP_LOGI("comb_bench",
             "N=%d iters=%d  C=%lu (%.1f/call)  PIE=%lu (%.1f/call)  "
             "speedup=%.2fx  maxdiff=%ld",
             N, iters, (unsigned long)tc, (double)tc / iters,
             (unsigned long)tp, (double)tp / iters, (double)tc / (double)tp,
             (long)maxdiff);
    ESP_LOGI("comb_bench", "yc[0..7]= %ld %ld %ld %ld %ld %ld %ld %ld",
             (long)yc[0], (long)yc[1], (long)yc[2], (long)yc[3], (long)yc[4],
             (long)yc[5], (long)yc[6], (long)yc[7]);
}
#endif

#if CONFIG_OPUS_P4_KERNEL_ASM_VERIFY

enum {
    VERIFY_ACTIVE,
    VERIFY_ACCEPTED,
    VERIFY_C_FALLBACK,
};

static unsigned s_anti_collapse_verify_mode;
static uint32_t s_anti_collapse_verify_calls;
static uint32_t s_verify_failures;

static bool anti_collapse_noise_matches(const float *x, int n, int stride,
                                        float r, uint32_t seed,
                                        uint32_t actual_seed)
{
    for (int i = 0; i < n; i++) {
        float expected;
        seed = 1664525u * seed + 1013904223u;
        expected = (seed & 0x8000u) ? r : -r;
        if (memcmp(&x[i * stride], &expected, sizeof(expected)) != 0)
            return false;
    }
    return seed == actual_seed;
}

static uint32_t anti_collapse_noise_f32_verify(float *x, int n, int stride,
                                               float r, uint32_t seed)
{
    if (s_anti_collapse_verify_mode == VERIFY_C_FALLBACK)
        return opus_p4_anti_collapse_noise_f32_c(x, n, stride, r, seed);

    uint32_t result = opus_p4_anti_collapse_noise_f32_p4(x, n, stride, r, seed);
    if (s_anti_collapse_verify_mode == VERIFY_ACCEPTED)
        return result;

    if (!anti_collapse_noise_matches(x, n, stride, r, seed, result)) {
        s_verify_failures++;
        s_anti_collapse_verify_mode = VERIFY_C_FALLBACK;
        ESP_LOGE("opus_p4", "anti-collapse asm mismatch; using portable C");
        return opus_p4_anti_collapse_noise_f32_c(x, n, stride, r, seed);
    }

    if (++s_anti_collapse_verify_calls >= CONFIG_OPUS_P4_VERIFY_CALLS)
        s_anti_collapse_verify_mode = VERIFY_ACCEPTED;
    return result;
}

#endif

/*
 * The dispatch object exists from the first integration step so codec call
 * sites do not change when PIE kernels replace individual C functions.
 */
static const opus_kernel_ops_t s_ops = {
    .inner_prod_f32 = opus_p4_inner_prod_f32_c,
#if CONFIG_OPUS_P4_KERNEL_ASM_VERIFY
    .anti_collapse_noise_f32 = anti_collapse_noise_f32_verify,
    .name = "p4-asm-abi-scaffold+verify",
#elif CONFIG_OPUS_P4_KERNEL_ASM
    .anti_collapse_noise_f32 = opus_p4_anti_collapse_noise_f32_p4,
    .name = "p4-asm-abi-scaffold",
#else
    .anti_collapse_noise_f32 = opus_p4_anti_collapse_noise_f32_c,
    .name = "portable-c",
#endif
};

float opus_p4_inner_prod_f32(const float *a, const float *b, int n)
{
    return s_ops.inner_prod_f32(a, b, n);
}

uint32_t opus_p4_anti_collapse_noise_f32(float *x, int n, int stride, float r,
                                         uint32_t seed)
{
    return s_ops.anti_collapse_noise_f32(x, n, stride, r, seed);
}

const char *opus_p4_kernel_impl(void)
{
#if CONFIG_OPUS_P4_KERNEL_ASM_VERIFY
    if (s_anti_collapse_verify_mode == VERIFY_C_FALLBACK)
        return "portable-c(fallback)";
    if (s_anti_collapse_verify_mode == VERIFY_ACCEPTED)
        return "p4-asm-abi-scaffold(verified)";
#endif
    return s_ops.name;
}

uint32_t opus_p4_kernel_verify_failures(void)
{
#if CONFIG_OPUS_P4_KERNEL_ASM_VERIFY
    return s_verify_failures;
#else
    return 0;
#endif
}

#if CONFIG_OPUS_P4_FUNCTION_PROFILE

#define PROFILE_SLOTS 1024
#define PROFILE_STACK_DEPTH 128

typedef struct {
    uintptr_t function;
    uint64_t self_cycles;
    uint32_t calls;
} profile_slot_t;

typedef struct {
    profile_slot_t *slot;
    uint32_t self_start;
} profile_frame_t;

static profile_slot_t *s_profile;
static profile_frame_t *s_stack;
static unsigned s_depth;
static bool s_enabled;
static uint32_t s_dropped;

static profile_slot_t *profile_slot(uintptr_t function)
    __attribute__((no_instrument_function));
static profile_slot_t *profile_slot(uintptr_t function)
{
    unsigned index = (unsigned)((function >> 2) & (PROFILE_SLOTS - 1));
    for (unsigned probe = 0; probe < PROFILE_SLOTS; probe++) {
        profile_slot_t *slot = &s_profile[(index + probe) & (PROFILE_SLOTS - 1)];
        if (slot->function == function || slot->function == 0) {
            if (slot->function == 0)
                slot->function = function;
            return slot;
        }
    }
    s_dropped++;
    return NULL;
}

void __cyg_profile_func_enter(void *function, void *caller)
    __attribute__((no_instrument_function));
void __cyg_profile_func_enter(void *function, void *caller)
{
    (void)caller;
    if (!s_enabled)
        return;

    uint32_t now = esp_cpu_get_cycle_count();
    if (s_depth > 0) {
        profile_frame_t *parent = &s_stack[s_depth - 1];
        parent->slot->self_cycles += (uint32_t)(now - parent->self_start);
    }
    if (s_depth >= PROFILE_STACK_DEPTH) {
        s_dropped++;
        return;
    }

    profile_slot_t *slot = profile_slot((uintptr_t)function);
    if (!slot)
        return;
    slot->calls++;
    s_stack[s_depth++] = (profile_frame_t){ .slot = slot, .self_start = now };
}

void __cyg_profile_func_exit(void *function, void *caller)
    __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void *function, void *caller)
{
    (void)function;
    (void)caller;
    if (!s_enabled || s_depth == 0)
        return;

    uint32_t now = esp_cpu_get_cycle_count();
    profile_frame_t *frame = &s_stack[--s_depth];
    frame->slot->self_cycles += (uint32_t)(now - frame->self_start);
    if (s_depth > 0)
        s_stack[s_depth - 1].self_start = now;
}

void opus_p4_profile_reset(void)
{
    s_enabled = false;
    if (!s_profile) {
        s_profile = heap_caps_calloc(PROFILE_SLOTS, sizeof(*s_profile),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_stack = heap_caps_calloc(PROFILE_STACK_DEPTH, sizeof(*s_stack),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_profile || !s_stack) {
        ESP_LOGE("opus_profile", "PSRAM allocation failed");
        return;
    }
    memset(s_profile, 0, PROFILE_SLOTS * sizeof(*s_profile));
    s_depth = 0;
    s_dropped = 0;
    s_enabled = true;
}

void opus_p4_profile_dump(int top_n)
{
    static const char *TAG = "opus_profile";
    s_enabled = false;
    if (!s_profile)
        return;
    uint64_t total_self_cycles = 0;
    for (unsigned i = 0; i < PROFILE_SLOTS; i++)
        total_self_cycles += s_profile[i].self_cycles;
    ESP_LOGI(TAG, "total_self=%" PRIu64, total_self_cycles);
    for (int rank = 0; rank < top_n; rank++) {
        profile_slot_t *best = NULL;
        for (unsigned i = 0; i < PROFILE_SLOTS; i++) {
            if (s_profile[i].self_cycles &&
                (!best || s_profile[i].self_cycles > best->self_cycles))
                best = &s_profile[i];
        }
        if (!best)
            break;
        ESP_LOGI(TAG, "%02d addr=0x%" PRIxPTR " self=%" PRIu64 " calls=%" PRIu32,
                 rank + 1, best->function, best->self_cycles, best->calls);
        best->self_cycles = 0;
    }
    ESP_LOGI(TAG, "dropped=%" PRIu32, s_dropped);
}

#else

void opus_p4_profile_reset(void)
{
}

void opus_p4_profile_dump(int top_n)
{
    (void)top_n;
}

#endif
