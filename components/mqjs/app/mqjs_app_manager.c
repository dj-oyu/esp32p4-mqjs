/*
 * App Manager — App record table (migration Phase 2).
 *
 * Separates the logical App (name, lifecycle state, kind, last-active)
 * from the Worker that executes it (JSContext, arena, bindings — still
 * owned by mqjs_runtime.c). Records persist across stop: a stopped app
 * still EXISTS here, which is the substrate Phase 3 (policy) and
 * Phase 4 (eviction / LRU) build on.
 *
 * Single-writer: every mutation happens on the js_task via the hooks in
 * mqjs_app_manager_internal.h. No locking by design (the same rule the
 * whole runtime follows). Portable C — compiled into the PC runner too,
 * so record transitions are testable off-device.
 */
#include <string.h>
#include <stdio.h>

#include "mqjs_app_manager.h"
#include "mqjs_app_manager_internal.h"

/* Sized comfortably past the 4 workers + the store catalog mirror (24):
   records are 64-odd bytes, this is internal-RAM cheap. */
#define MQJS_APP_RECORDS 24

static mqjs_app_snapshot_t s_records[MQJS_APP_RECORDS];
static int s_used[MQJS_APP_RECORDS];
static mqjs_app_id_t s_next_id = 1;

void mqjs_apps_init(void)
{
    memset(s_records, 0, sizeof s_records);
    memset(s_used, 0, sizeof s_used);
    s_next_id = 1;
}

void mqjs_apps_update(int64_t now_ms)
{
    /* Phase 2: nothing periodic yet. Phase 3+ (restart policy, idle
       stop) hangs its bookkeeping here. */
    (void)now_ms;
}

static int rec_find(const char *name)
{
    for (int i = 0; i < MQJS_APP_RECORDS; i++)
        if (s_used[i] && !strcmp(s_records[i].name, name))
            return i;
    return -1;
}

/* Free entry, else evict the longest-stopped record. RUNNING records
   are never evicted (there are at most MQJS_MAX_WORKERS of them, the
   table is far larger). */
static int rec_alloc(void)
{
    int oldest = -1;
    for (int i = 0; i < MQJS_APP_RECORDS; i++) {
        if (!s_used[i])
            return i;
        if (s_records[i].state == MQJS_APP_STOPPED &&
            (oldest < 0 ||
             s_records[i].last_active_ms < s_records[oldest].last_active_ms))
            oldest = i;
    }
    return oldest; /* -1 only if all 24 records are somehow running */
}

void mqjs_app_record_on_start(const char *name, int worker,
                              mqjs_app_kind_t kind, int64_t now_ms)
{
    int i = rec_find(name);
    if (i < 0) {
        i = rec_alloc();
        if (i < 0)
            return;
        memset(&s_records[i], 0, sizeof s_records[i]);
        s_used[i] = 1;
        s_records[i].id = s_next_id++;
        snprintf(s_records[i].name, sizeof s_records[i].name, "%s", name);
    }
    s_records[i].state = MQJS_APP_RUNNING;
    s_records[i].kind = kind;
    s_records[i].view = MQJS_APP_VIEW_BACKGROUND; /* fg arrives via touch */
    s_records[i].last_active_ms = now_ms;
    /* worker index is internal: the snapshot type deliberately has no
       field for it (mqjs_app_manager.h contract). */
    (void)worker;
}

void mqjs_app_record_on_stop(const char *name, mqjs_app_stop_reason_t reason)
{
    (void)reason; /* Phase 4 (eviction/restart) starts caring */
    int i = rec_find(name);
    if (i < 0)
        return;
    s_records[i].state = MQJS_APP_STOPPED;
    s_records[i].view = MQJS_APP_VIEW_NONE;
}

void mqjs_app_record_on_rename(const char *old_name, const char *new_name)
{
    int i = rec_find(old_name);
    if (i < 0)
        return;
    int dup = rec_find(new_name);
    if (dup >= 0 && dup != i)
        s_used[dup] = 0; /* absorb a stale stopped record */
    snprintf(s_records[i].name, sizeof s_records[i].name, "%s", new_name);
}

void mqjs_app_record_touch(const char *name, int64_t now_ms)
{
    int i = rec_find(name);
    if (i >= 0)
        s_records[i].last_active_ms = now_ms;
}

void mqjs_app_record_set_view(const char *name, mqjs_app_view_t view)
{
    int i = rec_find(name);
    if (i >= 0 && s_records[i].state == MQJS_APP_RUNNING)
        s_records[i].view = view;
}

const mqjs_app_snapshot_t *mqjs_app_record_find(const char *name)
{
    int i = rec_find(name);
    return i < 0 ? NULL : &s_records[i];
}

int mqjs_apps_snapshot(mqjs_app_snapshot_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < MQJS_APP_RECORDS && n < max; i++)
        if (s_used[i])
            out[n++] = s_records[i];
    return n;
}

/* ---- request_* stubs (contract in mqjs_app_manager.h) --------------
   The cross-task request queue is Phase 3 scope (today every caller of
   start/stop already runs on the js_task via JS bindings). Stubbed so
   the component links against the full contract from day one. */
bool mqjs_apps_request_start(const char *name)   { (void)name; return false; }
bool mqjs_apps_request_open(const char *name)    { (void)name; return false; }
bool mqjs_apps_request_focus(const char *name)   { (void)name; return false; }
bool mqjs_apps_request_stop(const char *name, mqjs_app_stop_reason_t r)
                                                 { (void)name; (void)r; return false; }
bool mqjs_apps_request_suspend(const char *name) { (void)name; return false; }
bool mqjs_apps_request_resume(const char *name)  { (void)name; return false; }
