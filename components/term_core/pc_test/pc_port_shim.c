/*
 * pc_port_shim.c — a host term_port for run_pc, used only as a fallback.
 *
 * run_pc.c has no boot path: on the device main/ installs
 * term_port_freertos() and calls term_registry_init(), and something on
 * the LVGL frame tick calls term_registry_ui_drain(). Neither exists on
 * the PC, so without a stand-in every term.* binding answers
 * TERM_ERR_NOT_READY and the JS tests measure nothing.
 *
 * run_pc_tests.sh probes the plain build first and only links this file
 * when the probe says term is not usable. If the runtime grows its own
 * host port, this shim stops being compiled in and nothing here runs.
 *
 * It is a real port, not a fake: pthread mutex/cond, a worker thread that
 * plays the UI frame task (drain + blit tick) and the reaper, and plain
 * malloc for the one per-term block (§4.2). That matters, because the
 * property the JS tests exercise — "log now, snapshot a frame later" —
 * only exists if somebody actually drains.
 *
 * Installed from a constructor so run_pc.c needs no edit.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "term_port.h"
#include "term_registry.h"

#define SHIM_QUEUE 64
#define SHIM_TICK_MS 5

typedef struct {
    term_ui_job_fn fn;
    void          *arg;
} shim_job_t;

static pthread_mutex_t  q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   q_cond = PTHREAD_COND_INITIALIZER;
static shim_job_t       q[SHIM_QUEUE];
static int              q_head, q_tail;
static pthread_t        ui_thread;
static volatile int     running;

/* ------------------------------------------------------------------ time */

static int64_t shim_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void abstime_in(struct timespec *ts, uint32_t ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += (time_t)(ms / 1000u);
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* ----------------------------------------------------------------- mutex */

static void *shim_mutex_create(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof *m);
    if (!m) return NULL;
    pthread_mutex_init(m, NULL);
    return m;
}

static void shim_mutex_destroy(void *mv)
{
    pthread_mutex_t *m = (pthread_mutex_t *)mv;
    if (!m) return;
    pthread_mutex_destroy(m);
    free(m);
}

static bool shim_mutex_lock(void *mv, uint32_t timeout_ms)
{
    pthread_mutex_t *m = (pthread_mutex_t *)mv;
    struct timespec ts;
    if (!m) return false;
    if (timeout_ms == TERM_WAIT_FOREVER) return pthread_mutex_lock(m) == 0;
    if (timeout_ms == TERM_WAIT_NONE)    return pthread_mutex_trylock(m) == 0;
    abstime_in(&ts, timeout_ms);
    return pthread_mutex_timedlock(m, &ts) == 0;
}

static void shim_mutex_unlock(void *mv)
{
    if (mv) pthread_mutex_unlock((pthread_mutex_t *)mv);
}

/* ---------------------------------------------------------------- signal */

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             set;
} shim_sig_t;

static void *shim_signal_create(void)
{
    shim_sig_t *s = (shim_sig_t *)malloc(sizeof *s);
    if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->set = 0;
    return s;
}

static void shim_signal_destroy(void *sv)
{
    shim_sig_t *s = (shim_sig_t *)sv;
    if (!s) return;
    pthread_cond_destroy(&s->c);
    pthread_mutex_destroy(&s->m);
    free(s);
}

static void shim_signal_set(void *sv)
{
    shim_sig_t *s = (shim_sig_t *)sv;
    if (!s) return;
    pthread_mutex_lock(&s->m);
    s->set = 1;
    pthread_cond_broadcast(&s->c);
    pthread_mutex_unlock(&s->m);
}

static bool shim_signal_wait(void *sv, uint32_t timeout_ms)
{
    shim_sig_t *s = (shim_sig_t *)sv;
    struct timespec ts;
    int rc = 0;
    if (!s) return false;
    abstime_in(&ts, timeout_ms == TERM_WAIT_FOREVER ? 10000u : timeout_ms);
    pthread_mutex_lock(&s->m);
    while (!s->set && rc == 0)
        rc = pthread_cond_timedwait(&s->c, &s->m, &ts);
    rc = s->set;
    pthread_mutex_unlock(&s->m);
    return rc != 0;
}

/* --------------------------------------------------------------- UI task */

static bool shim_ui_post(term_ui_job_fn fn, void *arg, uint32_t timeout_ms)
{
    int next;
    (void)timeout_ms;
    if (!fn) return false;
    pthread_mutex_lock(&q_lock);
    next = (q_tail + 1) % SHIM_QUEUE;
    if (next == q_head) { pthread_mutex_unlock(&q_lock); return false; }
    q[q_tail].fn = fn;
    q[q_tail].arg = arg;
    q_tail = next;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
    return true;
}

static bool shim_ui_is_current(void)
{
    return running && pthread_equal(pthread_self(), ui_thread);
}

/* --------------------------------------------------------------- memory */

static void *shim_mem_alloc(size_t size, unsigned flags)
{
    (void)flags;           /* one heap here; §4.1's PSRAM/L2 split is device-side */
    return malloc(size ? size : 1);
}

static void shim_mem_free(void *p) { free(p); }

/* ----------------------------------------------------------------- table */

static const term_port_t shim_port = {
    shim_mutex_create,
    shim_mutex_destroy,
    shim_mutex_lock,
    shim_mutex_unlock,
    shim_now_ms,
    shim_signal_create,
    shim_signal_destroy,
    shim_signal_set,
    shim_signal_wait,
    shim_ui_post,
    shim_ui_is_current,
    shim_mem_alloc,
    shim_mem_free,
    NULL,   /* reaper_wake: the worker polls, §3.1 allows that            */
    NULL,   /* log                                                        */
};

/* The worker plays two device tasks at once. They are separate on the
 * device (the UI frame task and the reaper task, §3.1) but neither may run
 * on js_task, and one host thread is enough to establish that. */
static void *shim_worker(void *unused)
{
    (void)unused;
    while (running) {
        shim_job_t job;
        struct timespec ts;
        int have = 0;

        pthread_mutex_lock(&q_lock);
        if (q_head == q_tail) {
            abstime_in(&ts, SHIM_TICK_MS);
            pthread_cond_timedwait(&q_cond, &q_lock, &ts);
        }
        if (q_head != q_tail) {
            job = q[q_head];
            q_head = (q_head + 1) % SHIM_QUEUE;
            have = 1;
        }
        pthread_mutex_unlock(&q_lock);

        /* Never with the queue lock held: these take the registry's own. */
        if (have) job.fn(job.arg);
        term_registry_ui_drain();
        term_registry_reap();
    }
    return NULL;
}

__attribute__((constructor))
static void shim_install(void)
{
    static term_create_opts_t console;
    static term_registry_config_t cfg;

    memset(&console, 0, sizeof console);
    console.name = "console";
    console.owner = TERM_OWNER_SYSTEM;
    console.mode = TERM_LOG;
    console.persist = true;
    console.cols = 80;
    console.rows = 24;

    memset(&cfg, 0, sizeof cfg);
    cfg.console = &console;

    if (!term_port_install(&shim_port)) return;
    if (term_registry_init(&cfg) != TERM_OK) return;

    running = 1;
    if (pthread_create(&ui_thread, NULL, shim_worker, NULL) != 0) running = 0;
}
