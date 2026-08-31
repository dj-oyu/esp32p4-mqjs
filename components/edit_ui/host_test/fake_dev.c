/*
 * The fake device edit_ui.c is linked against on a host: FreeRTOS, the
 * esp_timer, the heap, the PPA client, ui_tab5's canvas entry points and
 * the native surface registry.
 *
 * Two tricks let a single-threaded host run a task loop:
 *
 *   1. xTaskCreatePinnedToCore does not start anything. It stores the
 *      function, and records core/prio/stack so the test can assert spec
 *      §B.1 on them.
 *   2. xQueueReceive longjmp()s out when the queue is empty instead of
 *      blocking, so fake_pump() enters edit_task's endless loop, drains
 *      everything queued, and comes back.
 *
 * ui_tab5_cells_draw mimics the cell CONTRACT rather than the renderer:
 * one column per codepoint, a wide glyph having already been paid a
 * filler column by the caller (the CONT contract at the top of
 * ui_tab5.cpp). That is the part a presenter can get wrong; the glyph
 * raster is not.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ui_tab5.h"
#include "mqjs_native.h"
#include "fake_dev.h"

FakeUi fake;
mqjs_native_surface_t fake_surf;

/* ---- heap ---- */
void *heap_caps_aligned_alloc(size_t align, size_t n, uint32_t caps)
{
    (void)caps;
    void *p = NULL;
    if (align < sizeof(void *))
        align = sizeof(void *);
    if (posix_memalign(&p, align, n) != 0)
        return NULL;
    return p;
}

void heap_caps_free(void *p) { free(p); }

/* 90 KB: what a Tab5 boot log showed after the size diet. Above the 40 KB
   threshold in spec §A.3, so the staging stays private — which is the
   configuration this test exercises. */
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 92160; }

/* ---- clock: monotonic, 7 us a read, so no stage measures exactly 0 ---- */
static int64_t s_now = 1000000;
int64_t esp_timer_get_time(void) { return s_now += 7; }

/* ---- timers ---- */
struct esp_timer { esp_timer_cb_t cb; void *arg; int armed; }; /* 1 once, 2 periodic */
static struct esp_timer s_timers[8];
static int s_ntimers;

esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out)
{
    if (s_ntimers == (int)(sizeof(s_timers) / sizeof(s_timers[0])))
        return ESP_FAIL;
    struct esp_timer *t = &s_timers[s_ntimers++];
    t->cb = a->callback;
    t->arg = a->arg;
    t->armed = 0;
    *out = t;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    (void)us; t->armed = 1; return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us)
{
    (void)us; t->armed = 2; return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t t) { t->armed = 0; return ESP_OK; }
esp_err_t esp_timer_delete(esp_timer_handle_t t) { t->armed = 0; return ESP_OK; }

void fake_fire_timers(void)
{
    for (int i = 0; i < s_ntimers; i++) {
        if (!s_timers[i].armed)
            continue;
        if (s_timers[i].armed == 1)
            s_timers[i].armed = 0;   /* one-shot: disarm before firing, so a
                                        callback that re-arms is visible */
        s_timers[i].cb(s_timers[i].arg);
    }
}

/* ---- queue ---- */
struct QueueDef { unsigned cap, item, head, tail, n; unsigned char *buf; };
static jmp_buf s_jb;
static int s_in_task;

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    struct QueueDef *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    q->cap = len;
    q->item = item;
    q->buf = calloc(len, item);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q->n == q->cap)
        return pdFALSE;
    memcpy(q->buf + (size_t)q->tail * q->item, item, q->item);
    q->tail = (q->tail + 1) % q->cap;
    q->n++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (q->n == 0) {
        if (s_in_task)
            longjmp(s_jb, 1);    /* "block forever" becomes "return" */
        return pdFALSE;
    }
    memcpy(item, q->buf + (size_t)q->head * q->item, q->item);
    q->head = (q->head + 1) % q->cap;
    q->n--;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t q)
{
    if (q) { free(q->buf); free(q); }
}

/* ---- task ---- */
static TaskFunction_t s_fn;
static void *s_arg;

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core)
{
    (void)name;
    s_fn = fn;
    s_arg = arg;
    fake.task_core = core;
    fake.task_prio = (int)prio;
    fake.task_stack = stack;
    *out = (TaskHandle_t)(void *)1;
    return pdPASS;
}

void fake_pump(void)
{
    if (!s_fn)
        return;
    s_in_task = 1;
    if (setjmp(s_jb) == 0)
        s_fn(s_arg);
    s_in_task = 0;
}

/* ---- PPA ---- */
esp_err_t ppa_register_client(const ppa_client_config_t *c, ppa_client_handle_t *o)
{
    (void)c;
    *o = (ppa_client_handle_t)(void *)2;
    return ESP_OK;
}

esp_err_t ppa_unregister_client(ppa_client_handle_t c) { (void)c; return ESP_OK; }

/* ---- ui_tab5 ---- */
void ui_tab5_canvas_size(int *w, int *h) { *w = fake.canvas_w; *h = fake.canvas_h; }
void ui_tab5_cell_size(int *w, int *h) { *w = 9; *h = 24; }   /* HackGen 9x24 */
int  ui_tab5_kb_reserved(int mode) { (void)mode; return fake.kb_reserved; }
void ui_tab5_ime_face(int face) { fake.ime_face = face; }

bool ui_tab5_cmd(const ui_cmd_t *c)
{
    if (fake.ncmds < (int)(sizeof(fake.cmds) / sizeof(fake.cmds[0])))
        fake.cmds[fake.ncmds++] = c->op;
    return true;
}

static void put_cell(int row, int col, const char *utf8, size_t len, uint32_t fg)
{
    if (row < 0 || row >= FAKE_ROWS || col < 0 || col >= FAKE_COLS)
        return;
    size_t n = len < 7 ? len : 7;
    memcpy(fake.grid[row][col], utf8, n);
    fake.grid[row][col][n] = 0;
    fake.fg[row][col] = fg;
}

bool ui_tab5_cells_draw(const ui_cells_draw_t *d)
{
    if (!fake.canvas_up)
        return false;                     /* the canvas lv_obj is still hidden */
    if (fake.ndraws < (int)(sizeof(fake.draws) / sizeof(fake.draws[0]))) {
        fake.draws[fake.ndraws].row = d->row;
        fake.draws[fake.ndraws].col = d->col;
        fake.draws[fake.ndraws].ncells = d->ncells;
        fake.ndraws++;
    }
    if (d->row >= 0 && d->row < 64)
        fake.rows_touched |= (uint64_t)1 << d->row;

    for (int i = 0; i < d->ncells; i++)
        put_cell(d->row, d->col + i, " ", 1, d->bg);  /* the run's background */

    const char *p = d->utf8;
    size_t left = d->len;
    int col = d->col;
    while (left > 0 && col < d->col + d->ncells) {
        unsigned char b = (unsigned char)p[0];
        size_t n = 1;
        if (b >= 0xF0) n = 4;
        else if (b >= 0xE0) n = 3;
        else if (b >= 0xC0) n = 2;
        if (n > left)
            n = left;
        put_cell(d->row, col, p, n, d->fg);
        p += n;
        left -= n;
        col++;                            /* one column per codepoint */
    }
    return true;
}

void ui_tab5_canvas_fill(int x, int y, int w, int h, uint32_t rgb)
{
    (void)rgb;
    if (fake.nfills < (int)(sizeof(fake.fills) / sizeof(fake.fills[0]))) {
        FakeRect r = { x, y, w, h };
        fake.fills[fake.nfills++] = r;
    }
}

void ui_tab5_canvas_invalidate(int x, int y, int w, int h)
{
    if (fake.ninval < (int)(sizeof(fake.inval) / sizeof(fake.inval[0]))) {
        FakeRect r = { x, y, w, h };
        fake.inval[fake.ninval++] = r;
    }
}

/* ---- native surface registry ---- */
int  mqjs_native_register(const mqjs_native_surface_t *s) { fake_surf = *s; return 3; }
void mqjs_native_focus(int id) { (void)id; fake.focus_calls++; }
bool mqjs_native_is_fg(int id) { (void)id; return true; }

void fake_reset_marks(void)
{
    fake.ninval = 0;
    fake.nfills = 0;
    fake.ndraws = 0;
    fake.ncmds = 0;
    fake.rows_touched = 0;
}
