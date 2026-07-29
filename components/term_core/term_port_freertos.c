/*
 * term_port_freertos.c — the device half of the term_port seam.
 *
 * docs/term-design.md §11.2. FreeRTOS mutexes and binary semaphores,
 * esp_timer for the clock, ui_tab5's UI-task work seam, and
 * heap_caps PSRAM for the one block per term (§4.1/§4.3: the grid does
 * not go into the SRAM the size diet won back).
 *
 * The whole file is ESP_PLATFORM-guarded so the host test runner, which
 * compiles every source in this directory, can build it to nothing.
 *
 * It also owns the reaper task, which the registry deliberately does
 * not: term_port.h keeps task creation out of the seam so the registry
 * never becomes a second scheduler. Stage 2 of the quiesce is a plain
 * polling pass that somebody has to call; here that somebody is a
 * low-priority task woken either by the 250 ms period or by
 * reaper_wake().
 */

/* Not-empty-translation-unit insurance for the host build. */
typedef int term_port_freertos_tu_t;

#ifdef ESP_PLATFORM

#include "term_port.h"
#include "term_registry.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "ui_tab5.h"

static const char *TAG = "term";

/* ------------------------------------------------------------------ */
/* mutex                                                               */
/* ------------------------------------------------------------------ */

static void *fp_mutex_create(void)
{
    /* A plain mutex, not a recursive one: the registry never re-enters
       (term_port.h). Mutex, not binary semaphore, so the UI task gets
       priority inheritance over a JS worker that holds the table. */
    return (void *)xSemaphoreCreateMutex();
}

static void fp_mutex_destroy(void *m)
{
    if (m)
        vSemaphoreDelete((SemaphoreHandle_t)m);
}

static TickType_t to_ticks(uint32_t ms)
{
    if (ms == TERM_WAIT_FOREVER)
        return portMAX_DELAY;
    if (ms == TERM_WAIT_NONE)
        return 0;
    return pdMS_TO_TICKS(ms);
}

static bool fp_mutex_lock(void *m, uint32_t timeout_ms)
{
    if (!m)
        return false;
    return xSemaphoreTake((SemaphoreHandle_t)m, to_ticks(timeout_ms)) == pdTRUE;
}

static void fp_mutex_unlock(void *m)
{
    if (m)
        xSemaphoreGive((SemaphoreHandle_t)m);
}

/* ------------------------------------------------------------------ */
/* clock                                                               */
/* ------------------------------------------------------------------ */

static int64_t fp_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ------------------------------------------------------------------ */
/* signal (the bounded join of §7.2, and nothing else)                 */
/* ------------------------------------------------------------------ */

static void *fp_signal_create(void)
{
    return (void *)xSemaphoreCreateBinary();
}

static void fp_signal_destroy(void *s)
{
    if (s)
        vSemaphoreDelete((SemaphoreHandle_t)s);
}

static void fp_signal_set(void *s)
{
    if (s)
        xSemaphoreGive((SemaphoreHandle_t)s);
}

static bool fp_signal_wait(void *s, uint32_t timeout_ms)
{
    if (!s)
        return false;
    return xSemaphoreTake((SemaphoreHandle_t)s, to_ticks(timeout_ms)) == pdTRUE;
}

/* ------------------------------------------------------------------ */
/* UI frame task                                                       */
/* ------------------------------------------------------------------ */

#if CONFIG_MQJS_TAB5_UI

static bool fp_ui_post(term_ui_job_fn fn, void *arg, uint32_t timeout_ms)
{
    return ui_tab5_post_job((ui_tab5_job_fn)fn, arg, timeout_ms);
}

static bool fp_ui_is_current(void)
{
    return ui_tab5_is_ui_task();
}

#else /* headless build (Stamp-P4): there is no frame task */

/*
 * With no UI there is nothing to serialise against and nothing to blit:
 * the reaper task does the drain (see reaper_task below) and jobs run on
 * the caller. Reporting "you are the UI task" is honest here — the only
 * writer of a core is whoever calls, and the registry's lock still
 * serialises them.
 */
static bool fp_ui_post(term_ui_job_fn fn, void *arg, uint32_t timeout_ms)
{
    (void)timeout_ms;
    fn(arg);
    return true;
}

static bool fp_ui_is_current(void)
{
    return true;
}

#endif /* CONFIG_MQJS_TAB5_UI */

/* ------------------------------------------------------------------ */
/* memory (§4.1: PSRAM, one block per term, never reallocated)         */
/* ------------------------------------------------------------------ */

static void *fp_mem_alloc(size_t size, unsigned flags)
{
    uint32_t caps = (flags & TERM_MEM_INTERNAL) ? MALLOC_CAP_INTERNAL
                                                : MALLOC_CAP_SPIRAM;
    /* term_core_init requires 8-byte alignment; heap_caps_malloc only
       promises 4. heap_caps_free is the correct release for an aligned
       allocation on IDF 5+. */
    void *p = heap_caps_aligned_alloc(8, size, caps | MALLOC_CAP_8BIT);
    if (!p && !(flags & TERM_MEM_INTERNAL))
        ESP_LOGE(TAG, "term block alloc failed (%u bytes, PSRAM)",
                 (unsigned)size);
    return p;
}

static void fp_mem_free(void *p)
{
    heap_caps_free(p);
}

/* ------------------------------------------------------------------ */
/* reaper                                                              */
/* ------------------------------------------------------------------ */

#define TERM_REAP_PERIOD_MS 250

static SemaphoreHandle_t s_reap_wake;
static TaskHandle_t s_reap_task;

static void fp_reaper_wake(void)
{
    if (s_reap_wake)
        xSemaphoreGive(s_reap_wake);
}

static void reaper_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Period OR nudge — never a blind delay for a shutdown join
           (the microlink lesson, §3.1). The period alone would already
           be correct; the nudge only shortens the latency. */
        xSemaphoreTake(s_reap_wake, pdMS_TO_TICKS(TERM_REAP_PERIOD_MS));
#if !CONFIG_MQJS_TAB5_UI
        term_registry_ui_drain(); /* headless: no frame task exists */
#endif
        term_registry_reap();
    }
}

/* ------------------------------------------------------------------ */
/* the table                                                           */
/* ------------------------------------------------------------------ */

static void fp_log(int level, const char *tag, const char *msg)
{
    switch (level) {
    case 1: ESP_LOGE(TAG, "%s: %s", tag, msg); break;
    case 2: ESP_LOGW(TAG, "%s: %s", tag, msg); break;
    case 3: ESP_LOGI(TAG, "%s: %s", tag, msg); break;
    default: ESP_LOGD(TAG, "%s: %s", tag, msg); break;
    }
}

static const term_port_t s_port = {
    fp_mutex_create, fp_mutex_destroy, fp_mutex_lock, fp_mutex_unlock,
    fp_now_ms,
    fp_signal_create, fp_signal_destroy, fp_signal_set, fp_signal_wait,
    fp_ui_post, fp_ui_is_current,
    fp_mem_alloc, fp_mem_free,
    fp_reaper_wake, fp_log,
};

const term_port_t *term_port_freertos(void)
{
    return &s_port;
}

void term_port_freertos_start_reaper(void)
{
    if (s_reap_task)
        return;
    if (!s_reap_wake)
        s_reap_wake = xSemaphoreCreateBinary();
    if (!s_reap_wake)
        return;
    /* Low priority, small stack: one pass is a walk over 8 slots under
       the table lock. Explicitly NOT js_task and NOT the UI task
       (term_registry.h "WHO RUNS WHAT"). */
    xTaskCreate(reaper_task, "term_reap", 3072, NULL, 2, &s_reap_task);
}

#endif /* ESP_PLATFORM */
