#include "opus_p4_kernels.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

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
