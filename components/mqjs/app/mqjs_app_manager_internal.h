/*
 * js_task-only side of the App Manager (migration Phase 2).
 *
 * These hooks mutate the App record table and must only be called from
 * the js_task (the single writer — see mqjs_app_manager.h). They are
 * how the runtime's Worker lifecycle feeds the records; nothing here
 * is callable from other tasks or exposed to JS directly.
 */
#pragma once

#include <stdint.h>
#include "mqjs_app_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Worker started running an app under `name`: find-or-create the record,
   state -> RUNNING, bind the worker index. `kind` classifies launcher
   (SYSTEM) vs everything else (APP) until Phase 3 policies exist. */
void mqjs_app_record_on_start(const char *name, int worker,
                              mqjs_app_kind_t kind, int64_t now_ms);

/* App stopped: record survives (Apps exist while stopped — that is the
   point of the split), state -> STOPPED, worker unbound. */
void mqjs_app_record_on_stop(const char *name,
                             mqjs_app_stop_reason_t reason);

/* sys.setAppName: the running app's identity changed. A stale STOPPED
   record under the new name is absorbed. */
void mqjs_app_record_on_rename(const char *old_name, const char *new_name);

/* Foreground/eviction bookkeeping: refresh last_active_ms. */
void mqjs_app_record_touch(const char *name, int64_t now_ms);

/* Screen-state axis (independent of the run state, see the migration
   doc): the foreground switch updates both sides. */
void mqjs_app_record_set_view(const char *name, mqjs_app_view_t view);

/* Read-only record lookup (js_task). Returns NULL when unknown. */
const mqjs_app_snapshot_t *mqjs_app_record_find(const char *name);

/* Copy up to `max` records into `out`, returns the count. Order is
   table order (stable while no record is evicted). */
int mqjs_apps_snapshot(mqjs_app_snapshot_t *out, int max);

#ifdef __cplusplus
}
#endif
